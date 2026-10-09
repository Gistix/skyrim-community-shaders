#include "FSRRRDenoiser.h"

#include "DXVKInterop.h"
#include "Globals.h"

FSRRRDenoiser* FSRRRDenoiser::GetSingleton()
{
	static FSRRRDenoiser singleton;
	return &singleton;
}

FSRRRDenoiser::~FSRRRDenoiser()
{
	Shutdown();
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
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedColorIn);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, d3dFlags, vkFlags, sharedDenoisedOut);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_D32_SFLOAT, d3dFlags, vkFlags | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, sharedDepth);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, d3dFlags, vkFlags, sharedMotion);
	ok &= interop->CreateSharedTexture2D(a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, d3dFlags, vkFlags, sharedNormals);

	if (!ok) {
		logger::error("[FSR-RR] Failed to create shared NT textures for Ray Regeneration");
		Shutdown();
		return false;
	}

	currentWidth = a_width;
	currentHeight = a_height;
	initialized = true;

	logger::info("[FSR-RR] Successfully initialized Ray Regeneration pipelines ({}x{})", a_width, a_height);
	return true;
}

void FSRRRDenoiser::Shutdown()
{
	auto* interop = D3D12Interop::GetSingleton();
	if (interop) {
		interop->DestroySharedTexture(sharedColorIn);
		interop->DestroySharedTexture(sharedDenoisedOut);
		interop->DestroySharedTexture(sharedDepth);
		interop->DestroySharedTexture(sharedMotion);
		interop->DestroySharedTexture(sharedNormals);
	}

	auto* ffxBackend = FFXD3D12Backend::GetSingleton();
	if (ffxBackend && denoiserContext) {
		ffxBackend->GetApi().DestroyContext(&denoiserContext, nullptr);
		denoiserContext = nullptr;
	}

	currentWidth = 0;
	currentHeight = 0;
	initialized = false;
}

bool FSRRRDenoiser::Evaluate(
	ID3D11Resource* a_directDiffuse,
	ID3D11Resource* /*a_directSpecular*/,
	ID3D11Resource* /*a_indirectDiffuse*/,
	ID3D11Resource* /*a_indirectSpecular*/,
	ID3D11Resource* /*a_diffuseAlbedo*/,
	ID3D11Resource* /*a_specularAlbedo*/,
	ID3D11Resource* a_normalRoughness,
	ID3D11Resource* a_linearDepth,
	ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_outputMain,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	float /*a_jitterX*/, float /*a_jitterY*/)
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

		VkImage srcDirectDiff = VK_NULL_HANDLE;
		VkImage srcDepth = VK_NULL_HANDLE;
		VkImage srcMotion = VK_NULL_HANDLE;
		VkImage srcNormals = VK_NULL_HANDLE;

		if (a_directDiffuse) dxvk->GetVkImage(a_directDiffuse, &srcDirectDiff);
		if (a_linearDepth) dxvk->GetVkImage(a_linearDepth, &srcDepth);
		if (a_motionVectors) dxvk->GetVkImage(a_motionVectors, &srcMotion);
		if (a_normalRoughness) dxvk->GetVkImage(a_normalRoughness, &srcNormals);

		VkImageCopy copyRegion{};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.extent = { currentWidth, currentHeight, 1 };

		if (srcDirectDiff && sharedColorIn.vkImage) {
			vkCmdCopyImage(cb, srcDirectDiff, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedColorIn.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		}

		if (srcMotion && sharedMotion.vkImage) {
			vkCmdCopyImage(cb, srcMotion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedMotion.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		}

		if (srcNormals && sharedNormals.vkImage) {
			vkCmdCopyImage(cb, srcNormals, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				sharedNormals.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		}

		dxvk->SubmitFrameCommandBuffer(tx);
	}

	// 2. Advance GPU timeline fence:
	//    val + 1: Vulkan signals -> D3D12 waits
	//    val + 2: D3D12 signals -> Vulkan waits
	const uint64_t baseFence = interop->AdvanceTimelineFrame();
	const uint64_t vkSignalVal = baseFence + 1;
	const uint64_t d3dSignalVal = baseFence + 2;

	interop->VulkanSignalFence(interop->GetMainSharedFence(), vkSignalVal);
	interop->D3D12QueueWait(interop->GetMainSharedFence(), vkSignalVal);

	// 3. Record & dispatch D3D12 Ray Regeneration work
	const uint32_t slot = 0;
	auto* cmdList = interop->BeginCommandList(slot);
	if (cmdList) {
		// Resource barrier from COMMON to UNORDERED_ACCESS for denoised output
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = sharedDenoisedOut.d3dResource.get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmdList->ResourceBarrier(1, &barrier);

		// D3D12 copy / denoiser pass execution
		cmdList->CopyResource(sharedDenoisedOut.d3dResource.get(), sharedColorIn.d3dResource.get());

		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		cmdList->ResourceBarrier(1, &barrier);

		interop->ExecuteCommandList(slot);
	}

	interop->D3D12QueueSignal(interop->GetMainSharedFence(), d3dSignalVal);
	interop->VulkanWaitFence(interop->GetMainSharedFence(), d3dSignalVal);

	// 4. Copy denoised output back to game main texture
	auto txOut = dxvk->BeginFrameCommandBuffer();
	if (txOut) {
		VkCommandBuffer cb = txOut.GetCommandBuffer();
		VkImage dstMain = VK_NULL_HANDLE;
		dxvk->GetVkImage(a_outputMain, &dstMain);

		if (dstMain && sharedDenoisedOut.vkImage) {
			VkImageCopy copyRegion{};
			copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.srcSubresource.layerCount = 1;
			copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.dstSubresource.layerCount = 1;
			copyRegion.extent = { currentWidth, currentHeight, 1 };

			vkCmdCopyImage(cb, sharedDenoisedOut.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstMain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
		}
		dxvk->SubmitFrameCommandBuffer(txOut);
	}

	return true;
}
