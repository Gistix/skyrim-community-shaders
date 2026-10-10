#include "FSRRRDenoiser.h"

#include "DXVKInterop.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Game.h"

#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

static void RecordImageBarrier(
	VkCommandBuffer cb,
	VkImage image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkAccessFlags srcAccess,
	VkAccessFlags dstAccess,
	VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT)
{
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspectMask;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier);
}

FSRRRDenoiser* FSRRRDenoiser::GetSingleton()
{
	static FSRRRDenoiser singleton;
	return &singleton;
}

FSRRRDenoiser::~FSRRRDenoiser()
{
	Shutdown();
}

bool FSRRRDenoiser::InitCompositePipeline(ID3D12Device* a_device)
{
	if (!a_device)
		return false;

	// 1. Create Root Signature (Table with 7 SRVs at t0..t6, 1 UAV at u0)
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 7;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].RegisterSpace = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;

	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].RegisterSpace = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = 7;

	D3D12_ROOT_PARAMETER rootParam{};
	rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParam.DescriptorTable.NumDescriptorRanges = 2;
	rootParam.DescriptorTable.pDescriptorRanges = ranges;
	rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
	rootSigDesc.NumParameters = 1;
	rootSigDesc.pParameters = &rootParam;
	rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	winrt::com_ptr<ID3DBlob> serializedRootSig;
	winrt::com_ptr<ID3DBlob> errorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSig.put(), errorBlob.put());
	if (FAILED(hr)) {
		logger::error("[FSR-RR] D3D12SerializeRootSignature failed: hr={:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	hr = a_device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(compositeRootSig.put()));
	if (FAILED(hr)) {
		logger::error("[FSR-RR] CreateRootSignature failed: hr={:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	// 2. Compile HLSL composition shader
	static const char* s_compositeHlsl = R"(
Texture2D<float4> g_DirectDiffuse    : register(t0);
Texture2D<float4> g_DirectSpecular   : register(t1);
Texture2D<float4> g_IndirectDiffuse  : register(t2);
Texture2D<float4> g_IndirectSpecular : register(t3);
Texture2D<float4> g_DiffuseAlbedo    : register(t4);
Texture2D<float4> g_SpecularAlbedo   : register(t5);
Texture2D<float4> g_Emissive         : register(t6);

RWTexture2D<float4> g_Output         : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 diffAlbedo = g_DiffuseAlbedo[pixel].xyz;
    float3 specAlbedo = g_SpecularAlbedo[pixel].xyz;

    float3 directDiff   = max(g_DirectDiffuse[pixel].xyz, 0.0f);
    float3 directSpec   = max(g_DirectSpecular[pixel].xyz, 0.0f);
    float3 indirectDiff = max(g_IndirectDiffuse[pixel].xyz, 0.0f);
    float3 indirectSpec = max(g_IndirectSpecular[pixel].xyz, 0.0f);
    float4 emissive     = g_Emissive[pixel];

    float3 composite = (directDiff + indirectDiff) * diffAlbedo
                     + (directSpec + indirectSpec) * specAlbedo
                     + max(emissive.xyz, 0.0f);

    g_Output[pixel] = float4(composite, emissive.w);
}
)";

	winrt::com_ptr<ID3DBlob> shaderBlob;
	hr = D3DCompile(
		s_compositeHlsl, strlen(s_compositeHlsl),
		"FSR_RR_Composite", nullptr, nullptr,
		"CSMain", "cs_5_0", 0, 0,
		shaderBlob.put(), errorBlob.put());
	if (FAILED(hr)) {
		if (errorBlob) {
			logger::error("[FSR-RR] D3DCompile composition shader failed: {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		return false;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = compositeRootSig.get();
	psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
	hr = a_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(compositePSO.put()));
	if (FAILED(hr)) {
		logger::error("[FSR-RR] CreateComputePipelineState failed: hr={:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	// 3. Create descriptor heap for 7 SRVs and 1 UAV
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.NumDescriptors = 8;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = a_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(compositeHeap.put()));
	if (FAILED(hr)) {
		logger::error("[FSR-RR] CreateDescriptorHeap failed: hr={:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	// 4. Create descriptors in heap
	auto handle = compositeHeap->GetCPUDescriptorHandleForHeapStart();
	const auto step = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	a_device->CreateShaderResourceView(sharedDenoisedDirectDiffuse.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedDenoisedDirectSpecular.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedDenoisedIndirectDiffuse.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedDenoisedIndirectSpecular.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedDiffuseAlbedo.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedSpecularAlbedo.d3dResource.get(), nullptr, handle);
	handle.ptr += step;
	a_device->CreateShaderResourceView(sharedEmissiveIn.d3dResource.get(), nullptr, handle);
	handle.ptr += step;

	a_device->CreateUnorderedAccessView(sharedFinalOut.d3dResource.get(), nullptr, nullptr, handle);

	return true;
}

bool FSRRRDenoiser::Initialize(uint32_t a_width, uint32_t a_height)
{
	if (initialized && currentWidth == a_width && currentHeight == a_height)
		return true;

	Shutdown();

	if (a_width == 0 || a_height == 0)
		return false;

	auto* interop = D3D12Interop::GetSingleton();
	if (!interop || !interop->IsAvailable()) {
		logger::warn("[FSR-RR] D3D12Interop not available");
		return false;
	}

	auto* ffxBackend = FFXD3D12Backend::GetSingleton();
	if (!ffxBackend || !ffxBackend->IsAvailable()) {
		if (!ffxBackend->Initialize(interop->GetDevice())) {
			logger::warn("[FSR-RR] FidelityFX D3D12 backend failed to initialize");
			return false;
		}
	}

	// 1. Create cross-API shared textures
	constexpr D3D12_RESOURCE_FLAGS d3dFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
	                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
	                                          D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	constexpr VkImageUsageFlags vkFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                      VK_IMAGE_USAGE_SAMPLED_BIT |
	                                      VK_IMAGE_USAGE_STORAGE_BIT;

	bool ok = true;
	// Inputs
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDirectDiffuseIn);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDirectSpecularIn);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedIndirectDiffuseIn);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedIndirectSpecularIn);

	// Denoised outputs
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDenoisedDirectDiffuse);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDenoisedDirectSpecular);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDenoisedIndirectDiffuse);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDenoisedIndirectSpecular);

	// G-buffers
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDiffuseAlbedo);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedSpecularAlbedo);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedNormals);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, d3dFlags, vkFlags, sharedLinearDepth);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, d3dFlags, vkFlags, sharedMotion);

	// Emissive input and Final composite output
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedEmissiveIn);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedFinalOut);

	if (!ok) {
		logger::error("[FSR-RR] Failed to create shared NT textures for Ray Regeneration");
		Shutdown();
		return false;
	}

	currentWidth = a_width;
	currentHeight = a_height;

	// 2. Initialize composite compute pipeline
	if (!InitCompositePipeline(interop->GetDevice())) {
		logger::error("[FSR-RR] Failed to initialize composition compute pipeline");
		Shutdown();
		return false;
	}

	// 3. Create FidelityFX Denoiser context
	auto* ffx = FFXD3D12Backend::GetSingleton();
	if (ffx && ffx->IsAvailable()) {
		ffxCreateBackendDX12Desc backendDesc = ffx->GetBackendDesc();

		ffxCreateContextDescDenoiser denoiserDesc{};
		denoiserDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
		denoiserDesc.header.pNext = &backendDesc.header;
		denoiserDesc.flags = 0;
		denoiserDesc.signalFlags = FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE |
		                           FFX_DENOISER_SIGNAL_DIRECT_SPECULAR |
		                           FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE |
		                           FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
		denoiserDesc.checkerboardSignalFlags = 0;
		denoiserDesc.maxRenderSize = { a_width, a_height };

		ffxReturnCode_t ret = ffx->GetApi().CreateContext(&denoiserContext, &denoiserDesc.header, nullptr);
		if (ret == FFX_API_RETURN_OK && denoiserContext) {
			logger::info("[FSR-RR] Created FidelityFX Denoiser context successfully");

			if (ffx->GetApi().Query) {
				ffxQueryGetProviderVersion verQuery{};
				verQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
				if (ffx->GetApi().Query(&denoiserContext, &verQuery.header) == FFX_API_RETURN_OK) {
					logger::info("[FSR-RR] Active provider: '{}' (versionId {:#x})",
						verQuery.versionName ? verQuery.versionName : "Unknown",
						verQuery.versionId);
				}
			}
		} else {
			logger::warn("[FSR-RR] ffxCreateContext for Denoiser returned code {}; using composition fallback", ret);
			denoiserContext = nullptr;
		}
	}

	initialized = true;
	logger::info("[FSR-RR] Successfully initialized Ray Regeneration pipelines ({}x{})", a_width, a_height);
	return true;
}

void FSRRRDenoiser::Shutdown()
{
	auto* interop = D3D12Interop::GetSingleton();
	if (interop) {
		interop->DestroySharedTexture(sharedDirectDiffuseIn);
		interop->DestroySharedTexture(sharedDirectSpecularIn);
		interop->DestroySharedTexture(sharedIndirectDiffuseIn);
		interop->DestroySharedTexture(sharedIndirectSpecularIn);

		interop->DestroySharedTexture(sharedDenoisedDirectDiffuse);
		interop->DestroySharedTexture(sharedDenoisedDirectSpecular);
		interop->DestroySharedTexture(sharedDenoisedIndirectDiffuse);
		interop->DestroySharedTexture(sharedDenoisedIndirectSpecular);

		interop->DestroySharedTexture(sharedDiffuseAlbedo);
		interop->DestroySharedTexture(sharedSpecularAlbedo);
		interop->DestroySharedTexture(sharedNormals);
		interop->DestroySharedTexture(sharedLinearDepth);
		interop->DestroySharedTexture(sharedMotion);

		interop->DestroySharedTexture(sharedEmissiveIn);
		interop->DestroySharedTexture(sharedFinalOut);
	}

	auto* ffxBackend = FFXD3D12Backend::GetSingleton();
	if (ffxBackend && denoiserContext) {
		ffxBackend->GetApi().DestroyContext(&denoiserContext, nullptr);
		denoiserContext = nullptr;
	}

	compositeHeap = nullptr;
	compositePSO = nullptr;
	compositeRootSig = nullptr;

	currentWidth = 0;
	currentHeight = 0;
	initialized = false;
}

bool FSRRRDenoiser::Evaluate(
	ID3D11Resource* a_directDiffuse,
	ID3D11Resource* a_directSpecular,
	ID3D11Resource* a_indirectDiffuse,
	ID3D11Resource* a_indirectSpecular,
	ID3D11Resource* a_diffuseAlbedo,
	ID3D11Resource* a_specularAlbedo,
	ID3D11Resource* a_normalRoughness,
	ID3D11Resource* a_linearDepth,
	ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_outputMain,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	float a_jitterX, float a_jitterY)
{
	if (!initialized || !a_outputMain)
		return false;

	if (a_renderWidth != currentWidth || a_renderHeight != currentHeight) {
		if (!Initialize(a_renderWidth, a_renderHeight))
			return false;
	}

	auto* interop = D3D12Interop::GetSingleton();
	auto* dxvk = DXVKInterop::GetSingleton();
	if (!interop || !dxvk)
		return false;

	// 1. Copy Vulkan / D3D11 source textures into shared Vulkan surfaces on the GPU
	auto tx = dxvk->BeginFrameCommandBuffer();
	if (tx) {
		VkCommandBuffer cb = tx.GetCommandBuffer();

		VkImageCopy copyRegion{};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.extent = { currentWidth, currentHeight, 1 };

		const auto copyHelper = [&](ID3D11Resource* res, D3D12Interop::SharedTexture& dstShared) {
			if (!res || !dstShared.vkImage)
				return;
			VkImage srcImg = VK_NULL_HANDLE;
			VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			dxvk->GetVkImage(res, &srcImg, &srcLayout);
			if (!srcImg)
				return;

			VkImageLayout curSrc = (srcLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? srcLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			RecordImageBarrier(cb, srcImg, curSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			RecordImageBarrier(cb, dstShared.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdCopyImage(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstShared.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			RecordImageBarrier(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, curSrc,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
			RecordImageBarrier(cb, dstShared.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		};

		copyHelper(a_directDiffuse, sharedDirectDiffuseIn);
		copyHelper(a_directSpecular, sharedDirectSpecularIn);
		copyHelper(a_indirectDiffuse, sharedIndirectDiffuseIn);
		copyHelper(a_indirectSpecular, sharedIndirectSpecularIn);

		copyHelper(a_diffuseAlbedo, sharedDiffuseAlbedo);
		copyHelper(a_specularAlbedo, sharedSpecularAlbedo);
		copyHelper(a_normalRoughness, sharedNormals);
		copyHelper(a_linearDepth, sharedLinearDepth);
		copyHelper(a_motionVectors, sharedMotion);

		copyHelper(a_outputMain, sharedEmissiveIn);
	}

	// 2. Advance GPU timeline fence:
	//    val + 1: Vulkan signals -> D3D12 waits
	//    val + 2: D3D12 signals -> Vulkan waits
	const uint64_t baseFence = interop->AdvanceTimelineFrame();
	const uint64_t vkSignalVal = baseFence + 1;
	const uint64_t d3dSignalVal = baseFence + 2;

	if (tx) {
		dxvk->SubmitFrameCommandBuffer(tx, interop->GetMainSharedFence().vkTimelineSemaphore, vkSignalVal);
	}
	interop->D3D12QueueWait(interop->GetMainSharedFence(), vkSignalVal);

	// 3. Record & dispatch D3D12 Ray Regeneration work
	const uint32_t slot = 0;
	auto* cmdList = interop->BeginCommandList(slot);
	if (cmdList) {
		// Transition input resources to NON_PIXEL_SHADER_RESOURCE
		D3D12_RESOURCE_BARRIER preBarriers[15]{};
		uint32_t numPreBarriers = 0;

		const auto addTransition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter) {
			if (!res)
				return;
			auto& b = preBarriers[numPreBarriers++];
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res;
			b.Transition.StateBefore = stateBefore;
			b.Transition.StateAfter = stateAfter;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		};

		addTransition(sharedDirectDiffuseIn.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedDirectSpecularIn.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedIndirectDiffuseIn.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedIndirectSpecularIn.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		addTransition(sharedDiffuseAlbedo.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedSpecularAlbedo.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedNormals.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedLinearDepth.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedMotion.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addTransition(sharedEmissiveIn.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		addTransition(sharedDenoisedDirectDiffuse.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		addTransition(sharedDenoisedDirectSpecular.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		addTransition(sharedDenoisedIndirectDiffuse.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		addTransition(sharedDenoisedIndirectSpecular.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		addTransition(sharedFinalOut.d3dResource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cmdList->ResourceBarrier(numPreBarriers, preBarriers);

		if (denoiserContext) {
			auto* ffx = FFXD3D12Backend::GetSingleton();

			ffxDispatchDescDenoiserDirectDiffuse descDirectDiff{};
			descDirectDiff.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
			descDirectDiff.signal.input = FFXD3D12Backend::CreateResource(
				sharedDirectDiffuseIn, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			descDirectDiff.signal.output = FFXD3D12Backend::CreateResource(
				sharedDenoisedDirectDiffuse, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			descDirectDiff.signal.checkerboardOrigin = 0;

			ffxDispatchDescDenoiserDirectSpecular descDirectSpec{};
			descDirectSpec.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
			descDirectSpec.signal.input = FFXD3D12Backend::CreateResource(
				sharedDirectSpecularIn, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			descDirectSpec.signal.output = FFXD3D12Backend::CreateResource(
				sharedDenoisedDirectSpecular, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			descDirectSpec.signal.checkerboardOrigin = 0;

			ffxDispatchDescDenoiserIndirectDiffuse descIndirDiff{};
			descIndirDiff.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
			descIndirDiff.signal.input = FFXD3D12Backend::CreateResource(
				sharedIndirectDiffuseIn, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			descIndirDiff.signal.output = FFXD3D12Backend::CreateResource(
				sharedDenoisedIndirectDiffuse, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			descIndirDiff.signal.checkerboardOrigin = 0;

			ffxDispatchDescDenoiserIndirectSpecular descIndirSpec{};
			descIndirSpec.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
			descIndirSpec.signal.input = FFXD3D12Backend::CreateResource(
				sharedIndirectSpecularIn, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			descIndirSpec.signal.output = FFXD3D12Backend::CreateResource(
				sharedDenoisedIndirectSpecular, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			descIndirSpec.signal.checkerboardOrigin = 0;

			// Link pNext chain
			descDirectDiff.header.pNext = &descDirectSpec.header;
			descDirectSpec.header.pNext = &descIndirDiff.header;
			descIndirDiff.header.pNext = &descIndirSpec.header;
			descIndirSpec.header.pNext = nullptr;

			ffxDispatchDescDenoiser dispatchDesc{};
			dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
			dispatchDesc.header.pNext = &descDirectDiff.header;
			dispatchDesc.commandList = cmdList;
			dispatchDesc.linearDepth = FFXD3D12Backend::CreateResource(
				sharedLinearDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.motionVectors = FFXD3D12Backend::CreateResource(
				sharedMotion, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.normals = FFXD3D12Backend::CreateResource(
				sharedNormals, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.diffuseAlbedo = FFXD3D12Backend::CreateResource(
				sharedDiffuseAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.specularAlbedo = FFXD3D12Backend::CreateResource(
				sharedSpecularAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
			dispatchDesc.motionVectorScale = { static_cast<float>(currentWidth), static_cast<float>(currentHeight), 1.0f };
			dispatchDesc.jitterOffsets = { -a_jitterX, -a_jitterY };

			const auto camPos = globals::game::frameBufferCached.GetCameraPosAdjust();
			static float3 s_lastCamPos{ camPos.x, camPos.y, camPos.z };
			if (globals::state && globals::state->ShouldResetHistory()) {
				s_lastCamPos = { camPos.x, camPos.y, camPos.z };
			}
			dispatchDesc.cameraPositionDelta = { camPos.x - s_lastCamPos.x, camPos.y - s_lastCamPos.y, camPos.z - s_lastCamPos.z };
			s_lastCamPos = { camPos.x, camPos.y, camPos.z };

			const auto viewMat = globals::game::frameBufferCached.GetCameraView().Transpose();
			const auto projMat = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();
			std::memcpy(&dispatchDesc.view, &viewMat, sizeof(FfxApiMatrix4x4));
			std::memcpy(&dispatchDesc.projection, &projMat, sizeof(FfxApiMatrix4x4));

			float camNear = 0.1f;
			float camFar = 10000.0f;
			if (globals::game::cameraNear && *globals::game::cameraNear > 0.0f) {
				camNear = *globals::game::cameraNear;
			}
			if (globals::game::cameraFar && *globals::game::cameraFar > 0.0f) {
				camFar = *globals::game::cameraFar;
			}
			dispatchDesc.linearDepthBounds = { -camFar, -camNear };
			dispatchDesc.renderSize = { currentWidth, currentHeight };

			dispatchDesc.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;
			if (globals::state && globals::state->ShouldResetHistory()) {
				dispatchDesc.flags |= FFX_DENOISER_DISPATCH_RESET;
			}

			ffxReturnCode_t ret = ffx->GetApi().Dispatch(&denoiserContext, &dispatchDesc.header);
			if (ret != FFX_API_RETURN_OK) {
				static ffxReturnCode_t lastRet = FFX_API_RETURN_OK;
				if (ret != lastRet) {
					logger::error("[FSR-RR] ffxDispatch failed with code {}", ret);
					lastRet = ret;
				}
			}

			// Transition 4 denoised outputs from UAV to SRV for composition pass
			D3D12_RESOURCE_BARRIER midBarriers[4]{};
			for (int i = 0; i < 4; ++i) {
				midBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				midBarriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				midBarriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				midBarriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			}
			midBarriers[0].Transition.pResource = sharedDenoisedDirectDiffuse.d3dResource.get();
			midBarriers[1].Transition.pResource = sharedDenoisedDirectSpecular.d3dResource.get();
			midBarriers[2].Transition.pResource = sharedDenoisedIndirectDiffuse.d3dResource.get();
			midBarriers[3].Transition.pResource = sharedDenoisedIndirectSpecular.d3dResource.get();
			cmdList->ResourceBarrier(4, midBarriers);
		} else {
			// Fallback: Copy raw inputs to denoised outputs
			cmdList->CopyResource(sharedDenoisedDirectDiffuse.d3dResource.get(), sharedDirectDiffuseIn.d3dResource.get());
			cmdList->CopyResource(sharedDenoisedDirectSpecular.d3dResource.get(), sharedDirectSpecularIn.d3dResource.get());
			cmdList->CopyResource(sharedDenoisedIndirectDiffuse.d3dResource.get(), sharedIndirectDiffuseIn.d3dResource.get());
			cmdList->CopyResource(sharedDenoisedIndirectSpecular.d3dResource.get(), sharedIndirectSpecularIn.d3dResource.get());

			D3D12_RESOURCE_BARRIER midBarriers[4]{};
			for (int i = 0; i < 4; ++i) {
				midBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				midBarriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				midBarriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				midBarriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			}
			midBarriers[0].Transition.pResource = sharedDenoisedDirectDiffuse.d3dResource.get();
			midBarriers[1].Transition.pResource = sharedDenoisedDirectSpecular.d3dResource.get();
			midBarriers[2].Transition.pResource = sharedDenoisedIndirectDiffuse.d3dResource.get();
			midBarriers[3].Transition.pResource = sharedDenoisedIndirectSpecular.d3dResource.get();
			cmdList->ResourceBarrier(4, midBarriers);
		}

		// Dispatch composition compute shader
		if (compositePSO && compositeRootSig && compositeHeap) {
			cmdList->SetPipelineState(compositePSO.get());
			cmdList->SetComputeRootSignature(compositeRootSig.get());
			ID3D12DescriptorHeap* heaps[] = { compositeHeap.get() };
			cmdList->SetDescriptorHeaps(1, heaps);
			cmdList->SetComputeRootDescriptorTable(0, compositeHeap->GetGPUDescriptorHandleForHeapStart());
			cmdList->Dispatch((currentWidth + 7) / 8, (currentHeight + 7) / 8, 1);
		}

		// Transition resources back to COMMON
		D3D12_RESOURCE_BARRIER postBarriers[15]{};
		uint32_t numPostBarriers = 0;
		const auto addPostTransition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES stateBefore) {
			if (!res)
				return;
			auto& b = postBarriers[numPostBarriers++];
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res;
			b.Transition.StateBefore = stateBefore;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		};

		addPostTransition(sharedFinalOut.d3dResource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		addPostTransition(sharedDirectDiffuseIn.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedDirectSpecularIn.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedIndirectDiffuseIn.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedIndirectSpecularIn.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		addPostTransition(sharedDiffuseAlbedo.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedSpecularAlbedo.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedNormals.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedLinearDepth.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedMotion.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedEmissiveIn.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		addPostTransition(sharedDenoisedDirectDiffuse.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedDenoisedDirectSpecular.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedDenoisedIndirectDiffuse.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addPostTransition(sharedDenoisedIndirectSpecular.d3dResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		cmdList->ResourceBarrier(numPostBarriers, postBarriers);

		interop->ExecuteCommandList(slot);
	}

	interop->D3D12QueueSignal(interop->GetMainSharedFence(), d3dSignalVal);
	interop->VulkanWaitFence(interop->GetMainSharedFence(), d3dSignalVal);

	// 4. Copy composited final output back to game main texture
	auto txOut = dxvk->BeginFrameCommandBuffer();
	if (txOut) {
		VkCommandBuffer cb = txOut.GetCommandBuffer();
		VkImage dstMain = VK_NULL_HANDLE;
		VkImageLayout dstMainLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dxvk->GetVkImage(a_outputMain, &dstMain, &dstMainLayout);

		if (dstMain && sharedFinalOut.vkImage) {
			VkImageCopy copyRegion{};
			copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.srcSubresource.layerCount = 1;
			copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.dstSubresource.layerCount = 1;
			copyRegion.extent = { currentWidth, currentHeight, 1 };

			VkImageLayout curDst = (dstMainLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? dstMainLayout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			RecordImageBarrier(cb, sharedFinalOut.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			RecordImageBarrier(cb, dstMain, curDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdCopyImage(cb, sharedFinalOut.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstMain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			RecordImageBarrier(cb, dstMain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, curDst,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
			RecordImageBarrier(cb, sharedFinalOut.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		}
		dxvk->SubmitFrameCommandBuffer(txOut);
	}

	return true;
}
