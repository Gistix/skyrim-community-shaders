#include "FSRRRDenoiser.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "Buffer.h"
#include "DxvkLoader.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#include <cstring>
#include <d3d11.h>
#include <filesystem>
#include <vector>

namespace
{
	struct alignas(16) FSRRRParams
	{
		uint32_t width;
		uint32_t height;
		float colorExponent;
		float padding;

		float cameraData[4]; // far, near, far-near, far*near
	};
	static_assert(sizeof(FSRRRParams) == 32);

	constexpr uint32_t kFramesInFlight = 3;
	constexpr const wchar_t* kPrepareShaderPath = L"Data\\Shaders\\Raytracing\\FSRRRPrepareCS.hlsl";
	constexpr const wchar_t* kCompositeShaderPath = L"Data\\Shaders\\Raytracing\\FSRRRCompositeCS.hlsl";
}

struct FSRRRDenoiser::Impl
{
	// Vulkan device handles borrowed from DXVK.
	DXVKInterop* dxvk = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamilyIndex = 0;
	IDXGIVkInteropDevice* interopDevice = nullptr;
	PFN_vkGetMemoryWin32HandleKHR getMemoryWin32Handle = nullptr;
	PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreWin32Handle = nullptr;

	// Vulkan host-visible staging buffers for 16-channel planar FP32 tensor.
	VkBuffer inputVk = VK_NULL_HANDLE;
	VkDeviceMemory inputMem = VK_NULL_HANDLE;
	void* inputMapped = nullptr;

	VkBuffer outputVk = VK_NULL_HANDLE;
	VkDeviceMemory outputMem = VK_NULL_HANDLE;
	void* outputMapped = nullptr;

	// Command ring.
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer copyInCmd[kFramesInFlight]{};
	VkCommandBuffer copyOutCmd[kFramesInFlight]{};
	VkFence frameFences[kFramesInFlight]{};
	uint32_t frameIndex = 0;

	// D3D11 side resources.
	winrt::com_ptr<ID3D11Buffer> inputTensorD3D;
	winrt::com_ptr<ID3D11Buffer> outputTensorD3D;
	winrt::com_ptr<ID3D11UnorderedAccessView> inputTensorUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> outputTensorSRV;
	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS;
	winrt::com_ptr<ID3D11Buffer> paramsCB;

	// Cached underlying Vulkan buffers of D3D11 structured buffers.
	VkBuffer inputSrcVk = VK_NULL_HANDLE;
	VkDeviceSize inputSrcOffset = 0;
	VkBuffer outputDstVk = VK_NULL_HANDLE;
	VkDeviceSize outputDstOffset = 0;

	// ONNX Runtime DirectML engine
	std::unique_ptr<Ort::Env> ortEnv;
	std::unique_ptr<Ort::SessionOptions> ortSessionOptions;
	std::unique_ptr<Ort::Session> ortSession;
	std::unique_ptr<Ort::MemoryInfo> ortMemoryInfo;
	bool ortReady = false;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t tensorByteSize = 0;
	bool resourcesReady = false;

	float colorDecodeExponent = 2.2f;
	bool initialized = false;

	void DestroyResources();
	bool CreateResources(uint32_t a_width, uint32_t a_height);

	uint32_t FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const;
	bool CreateStagingBuffer(uint32_t a_size, VkBuffer& a_buffer, VkDeviceMemory& a_memory, void** a_mapped);
	void DestroyStagingBuffer(VkBuffer& a_buffer, VkDeviceMemory& a_memory, void** a_mapped);
	void ResolveD3DBuffer(ID3D11Buffer* a_resource, VkBuffer& a_outBuffer, VkDeviceSize& a_outOffset);

	uint32_t AcquireFrame();
	void RecordCopyIn(uint32_t a_frame);
	void RecordCopyOut(uint32_t a_frame);
	void Submit(const VkSubmitInfo& a_submitInfo, VkFence a_fence);
};

FSRRRDenoiser::FSRRRDenoiser() :
	impl(std::make_unique<Impl>())
{}

FSRRRDenoiser::~FSRRRDenoiser()
{
	Shutdown();
}

bool FSRRRDenoiser::IsAvailable() const
{
	return impl && impl->initialized && impl->compositeCS && impl->prepareCS && impl->ortReady;
}

void FSRRRDenoiser::SetColorDecodeExponent(float a_exponent)
{
	if (impl)
		impl->colorDecodeExponent = a_exponent;
}

uint32_t FSRRRDenoiser::Impl::FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const
{
	VkPhysicalDeviceMemoryProperties memProperties{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
		if ((a_typeBits & (1u << i)) &&
			(memProperties.memoryTypes[i].propertyFlags & a_properties) == a_properties) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool FSRRRDenoiser::Impl::CreateStagingBuffer(uint32_t a_size, VkBuffer& a_buffer, VkDeviceMemory& a_memory, void** a_mapped)
{
	if (a_mapped)
		*a_mapped = nullptr;

	VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = a_size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(device, &bufferInfo, nullptr, &a_buffer) != VK_SUCCESS) {
		logger::error("[FSR-RR] Failed to create staging buffer");
		return false;
	}

	VkMemoryRequirements memReqs{};
	vkGetBufferMemoryRequirements(device, a_buffer, &memReqs);

	VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (allocInfo.memoryTypeIndex == UINT32_MAX) {
		allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	}

	if (allocInfo.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(device, &allocInfo, nullptr, &a_memory) != VK_SUCCESS) {
		logger::error("[FSR-RR] Failed to allocate host-visible memory for staging buffer");
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_buffer = VK_NULL_HANDLE;
		return false;
	}

	if (vkBindBufferMemory(device, a_buffer, a_memory, 0) != VK_SUCCESS) {
		logger::error("[FSR-RR] Failed to bind memory to staging buffer");
		vkFreeMemory(device, a_memory, nullptr);
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_memory = VK_NULL_HANDLE;
		a_buffer = VK_NULL_HANDLE;
		return false;
	}

	if (a_mapped) {
		if (vkMapMemory(device, a_memory, 0, a_size, 0, a_mapped) != VK_SUCCESS) {
			logger::error("[FSR-RR] Failed to map staging buffer memory");
			*a_mapped = nullptr;
			return false;
		}
	}

	return true;
}

void FSRRRDenoiser::Impl::DestroyStagingBuffer(VkBuffer& a_buffer, VkDeviceMemory& a_memory, void** a_mapped)
{
	if (a_mapped && *a_mapped && device != VK_NULL_HANDLE && a_memory != VK_NULL_HANDLE) {
		vkUnmapMemory(device, a_memory);
		*a_mapped = nullptr;
	}
	if (a_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_buffer = VK_NULL_HANDLE;
	}
	if (a_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, a_memory, nullptr);
		a_memory = VK_NULL_HANDLE;
	}
}

bool FSRRRDenoiser::Initialize()
{
	auto& self = *impl;
	if (self.initialized)
		return true;

	self.dxvk = DXVKInterop::GetSingleton();
	if (!self.dxvk || !self.dxvk->IsAvailable()) {
		logger::warn("[FSR-RR] DXVK Vulkan interop unavailable; FSR-RR disabled");
		return false;
	}

	self.instance = self.dxvk->GetInstance();
	self.physicalDevice = self.dxvk->GetPhysicalDevice();
	self.device = self.dxvk->GetDevice();
	self.queue = self.dxvk->GetQueue();
	self.queueFamilyIndex = self.dxvk->GetQueueFamilyIndex();
	self.interopDevice = self.dxvk->GetInteropDevice();

	if (!self.device || !self.queue || !self.interopDevice) {
		logger::warn("[FSR-RR] Missing Vulkan device/queue; FSR-RR disabled");
		return false;
	}

	const auto getDeviceProcAddr = self.dxvk->GetDeviceProcAddr();
	if (!getDeviceProcAddr) {
		logger::warn("[FSR-RR] vkGetDeviceProcAddr unavailable; FSR-RR disabled");
		return false;
	}

	self.getMemoryWin32Handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
		getDeviceProcAddr(self.device, "vkGetMemoryWin32HandleKHR"));
	self.getSemaphoreWin32Handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
		getDeviceProcAddr(self.device, "vkGetSemaphoreWin32HandleKHR"));

	// Resolve the runtime directory from the plugin location (CommunityShaders/bin)
	const auto runtimeDir = DxvkLoader::GetRuntimeDir();
	if (runtimeDir.empty()) {
		logger::warn("[FSR-RR] Could not resolve plugin runtime directory; FSR-RR disabled");
		return false;
	}
	SetDllDirectoryW(runtimeDir.c_str());

	const auto loadRuntimeModule = [&runtimeDir](const wchar_t* a_name) -> HMODULE {
		const std::filesystem::path path = runtimeDir / a_name;
		HMODULE handle = LoadLibraryW(path.c_str());
		if (!handle)
			logger::warn("[FSR-RR] LoadLibrary failed for {} (error {})", path.string(), ::GetLastError());
		return handle;
	};

	loadRuntimeModule(L"onnxruntime_providers_shared.dll");
	const HMODULE ortLib = loadRuntimeModule(L"onnxruntime.dll");
	if (!ortLib) {
		logger::warn("[FSR-RR] ONNX Runtime DLL unavailable; FSR-RR disabled");
		return false;
	}
	if (!GetProcAddress(ortLib, "OrtGetApiBase")) {
		logger::warn("[FSR-RR] OrtGetApiBase export missing (error {})", ::GetLastError());
		return false;
	}

	// Initialize ONNX Runtime with DirectML execution provider
	std::wstring modelPath;
	const std::vector<std::filesystem::path> candidatePaths = {
		L"Data\\Shaders\\Raytracing\\fsrrr_unet.onnx",
		L"Shaders\\Raytracing\\fsrrr_unet.onnx",
		runtimeDir.parent_path().parent_path() / L"Shaders" / L"Raytracing" / L"fsrrr_unet.onnx",
		L"SKSE\\Plugins\\CommunityShaders\\fsrrr_unet.onnx",
		L"features\\Raytracing\\Shaders\\Raytracing\\fsrrr_unet.onnx",
		L"build\\DEV-WITH-AUTO-DEPLOYMENT\\aio\\Shaders\\Raytracing\\fsrrr_unet.onnx"
	};

	for (const auto& path : candidatePaths) {
		if (std::filesystem::exists(path)) {
			modelPath = path.wstring();
			break;
		}
	}

	if (!modelPath.empty()) {
		try {
			self.ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FSRRR");
			self.ortSessionOptions = std::make_unique<Ort::SessionOptions>();
			self.ortSessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
			self.ortSessionOptions->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
			self.ortSessionOptions->SetIntraOpNumThreads(1);

			// Append DirectML Execution Provider on GPU adapter 0
			OrtSessionOptionsAppendExecutionProvider_DML(*self.ortSessionOptions, 0);

			self.ortSession = std::make_unique<Ort::Session>(*self.ortEnv, modelPath.c_str(), *self.ortSessionOptions);
			self.ortMemoryInfo = std::make_unique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
			self.ortReady = true;

			logger::info("[FSR-RR] ONNX Runtime DirectML neural denoiser loaded successfully from {}", std::filesystem::path(modelPath).string());
		} catch (const std::exception& e) {
			logger::error("[FSR-RR] Failed to initialize ONNX Runtime DirectML engine: {}", e.what());
			self.ortReady = false;
		}
	} else {
		logger::warn("[FSR-RR] Model 'fsrrr_unet.onnx' not found in shader search paths");
		self.ortReady = false;
	}

	// Compile D3D11 compute shaders.
	self.prepareCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(kPrepareShaderPath, {}, "cs_5_0")));
	self.compositeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(kCompositeShaderPath, {}, "cs_5_0")));
	if (!self.prepareCS || !self.compositeCS) {
		logger::error("[FSR-RR] Failed to compile FSR-RR compute shaders; FSR-RR disabled");
		return false;
	}

	auto* device = globals::d3d::device;
	if (!device) {
		logger::error("[FSR-RR] No D3D11 device; FSR-RR disabled");
		return false;
	}

	// Constant buffer for shader parameters.
	D3D11_BUFFER_DESC cbDesc{};
	cbDesc.ByteWidth = sizeof(FSRRRParams);
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&cbDesc, nullptr, self.paramsCB.put()))) {
		logger::error("[FSR-RR] Failed to create constant buffer");
		return false;
	}

	// Command pool + buffers + fences.
	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = self.queueFamilyIndex;
	if (vkCreateCommandPool(self.device, &poolInfo, nullptr, &self.commandPool) != VK_SUCCESS) {
		logger::error("[FSR-RR] Failed to create Vulkan command pool");
		return false;
	}

	VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocateInfo.commandPool = self.commandPool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = kFramesInFlight;
	if (vkAllocateCommandBuffers(self.device, &allocateInfo, self.copyInCmd) != VK_SUCCESS ||
		vkAllocateCommandBuffers(self.device, &allocateInfo, self.copyOutCmd) != VK_SUCCESS) {
		logger::error("[FSR-RR] Failed to allocate Vulkan command buffers");
		return false;
	}

	VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < kFramesInFlight; ++i) {
		if (vkCreateFence(self.device, &fenceInfo, nullptr, &self.frameFences[i]) != VK_SUCCESS) {
			logger::error("[FSR-RR] Failed to create Vulkan fences");
			return false;
		}
	}

	self.initialized = true;
	logger::info("[FSR-RR] FidelityFX Ray Regeneration (MLD) GPU denoiser initialized");
	return true;
}

void FSRRRDenoiser::Shutdown()
{
	if (!impl)
		return;

	auto& self = *impl;
	if (!self.initialized)
		return;

	self.DestroyResources();

	if (self.device != VK_NULL_HANDLE) {
		if (self.commandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(self.device, self.commandPool, nullptr);
			self.commandPool = VK_NULL_HANDLE;
		}
		for (auto& fence : self.frameFences) {
			if (fence != VK_NULL_HANDLE) {
				vkDestroyFence(self.device, fence, nullptr);
				fence = VK_NULL_HANDLE;
			}
		}
	}

	self.ortSession = nullptr;
	self.ortSessionOptions = nullptr;
	self.ortMemoryInfo = nullptr;
	self.ortEnv = nullptr;
	self.ortReady = false;

	self.prepareCS = nullptr;
	self.compositeCS = nullptr;
	self.paramsCB = nullptr;
	self.initialized = false;
}

void FSRRRDenoiser::Impl::ResolveD3DBuffer(ID3D11Buffer* a_resource, VkBuffer& a_outBuffer, VkDeviceSize& a_outOffset)
{
	a_outBuffer = VK_NULL_HANDLE;
	a_outOffset = 0;
	if (!dxvk || !a_resource)
		return;

	VkDeviceSize length = 0;
	if (!dxvk->GetVkBuffer(a_resource, &a_outBuffer, &a_outOffset, &length, nullptr)) {
		a_outBuffer = VK_NULL_HANDLE;
		a_outOffset = 0;
	}
}

void FSRRRDenoiser::Impl::DestroyResources()
{
	resourcesReady = false;

	inputTensorD3D = nullptr;
	outputTensorD3D = nullptr;
	inputTensorUAV = nullptr;
	outputTensorSRV = nullptr;

	DestroyStagingBuffer(inputVk, inputMem, &inputMapped);
	DestroyStagingBuffer(outputVk, outputMem, &outputMapped);

	inputSrcVk = VK_NULL_HANDLE;
	inputSrcOffset = 0;
	outputDstVk = VK_NULL_HANDLE;
	outputDstOffset = 0;

	width = 0;
	height = 0;
	tensorByteSize = 0;
}

bool FSRRRDenoiser::Impl::CreateResources(uint32_t a_width, uint32_t a_height)
{
	if (resourcesReady && width == a_width && height == a_height)
		return true;

	DestroyResources();

	auto* d3dDevice = globals::d3d::device;
	if (!d3dDevice || !device)
		return false;

	width = a_width;
	height = a_height;

	const uint32_t pixelCount = width * height;
	// 16 channels of 32-bit float per pixel = 64 bytes per pixel (planar NCHW format)
	tensorByteSize = pixelCount * 16 * sizeof(float);

	// D3D11 structured buffers: stride 4 bytes (float), element count = pixelCount * 16
	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = sizeof(float);
	bufferDesc.ByteWidth = tensorByteSize;

	if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, inputTensorD3D.put()))) {
		logger::error("[FSR-RR] Failed to create input tensor buffer");
		DestroyResources();
		return false;
	}

	if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, outputTensorD3D.put()))) {
		logger::error("[FSR-RR] Failed to create output tensor buffer");
		DestroyResources();
		return false;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = pixelCount * 16;
	if (FAILED(d3dDevice->CreateUnorderedAccessView(inputTensorD3D.get(), &uavDesc, inputTensorUAV.put()))) {
		logger::error("[FSR-RR] Failed to create input tensor UAV");
		DestroyResources();
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = pixelCount * 16;
	if (FAILED(d3dDevice->CreateShaderResourceView(outputTensorD3D.get(), &srvDesc, outputTensorSRV.put()))) {
		logger::error("[FSR-RR] Failed to create output tensor SRV");
		DestroyResources();
		return false;
	}

	// Cache underlying Vulkan buffers
	ResolveD3DBuffer(inputTensorD3D.get(), inputSrcVk, inputSrcOffset);
	ResolveD3DBuffer(outputTensorD3D.get(), outputDstVk, outputDstOffset);

	// Host-visible staging buffers for ONNX Runtime ML inference
	if (!CreateStagingBuffer(tensorByteSize, inputVk, inputMem, &inputMapped) ||
		!CreateStagingBuffer(tensorByteSize, outputVk, outputMem, &outputMapped)) {
		logger::error("[FSR-RR] Failed to create host-visible staging buffers");
		DestroyResources();
		return false;
	}

	resourcesReady = true;
	logger::info("[FSR-RR] Resources created ({}x{}, {:.2f} MB planar NCHW tensor footprint)",
		width, height, (tensorByteSize * 2) / (1024.0f * 1024.0f));
	return true;
}

uint32_t FSRRRDenoiser::Impl::AcquireFrame()
{
	const uint32_t frame = frameIndex;
	frameIndex = (frameIndex + 1) % kFramesInFlight;
	return frame;
}

void FSRRRDenoiser::Impl::RecordCopyIn(uint32_t a_frame)
{
	const VkCommandBuffer cb = copyInCmd[a_frame];
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &beginInfo);

	if (inputVk != VK_NULL_HANDLE && inputSrcVk != VK_NULL_HANDLE) {
		VkBufferMemoryBarrier barriers[2]{};
		barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].buffer = inputSrcVk;
		barriers[0].offset = inputSrcOffset;
		barriers[0].size = tensorByteSize;

		barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barriers[1].srcAccessMask = 0;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].buffer = inputVk;
		barriers[1].offset = 0;
		barriers[1].size = tensorByteSize;

		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr, 2, barriers, 0, nullptr);

		VkBufferCopy region{};
		region.srcOffset = inputSrcOffset;
		region.dstOffset = 0;
		region.size = tensorByteSize;
		vkCmdCopyBuffer(cb, inputSrcVk, inputVk, 1, &region);

		VkMemoryBarrier memoryBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memoryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
			1, &memoryBarrier, 0, nullptr, 0, nullptr);
	}

	vkEndCommandBuffer(cb);
}

void FSRRRDenoiser::Impl::RecordCopyOut(uint32_t a_frame)
{
	const VkCommandBuffer cb = copyOutCmd[a_frame];
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &beginInfo);

	if (outputVk != VK_NULL_HANDLE && outputDstVk != VK_NULL_HANDLE) {
		VkBufferMemoryBarrier barriers[2]{};
		barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].buffer = outputVk;
		barriers[0].offset = 0;
		barriers[0].size = tensorByteSize;

		barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barriers[1].srcAccessMask = 0;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].buffer = outputDstVk;
		barriers[1].offset = outputDstOffset;
		barriers[1].size = tensorByteSize;

		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr, 2, barriers, 0, nullptr);

		VkBufferCopy region{};
		region.srcOffset = 0;
		region.dstOffset = outputDstOffset;
		region.size = tensorByteSize;
		vkCmdCopyBuffer(cb, outputVk, outputDstVk, 1, &region);

		VkBufferMemoryBarrier outBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		outBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		outBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		outBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		outBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		outBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		outBarrier.buffer = outputDstVk;
		outBarrier.offset = outputDstOffset;
		outBarrier.size = tensorByteSize;

		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			0, nullptr, 1, &outBarrier, 0, nullptr);
	}

	vkEndCommandBuffer(cb);
}

void FSRRRDenoiser::Impl::Submit(const VkSubmitInfo& a_submitInfo, VkFence a_fence)
{
	if (!interopDevice)
		return;

	interopDevice->FlushRenderingCommands();
	interopDevice->LockSubmissionQueue();
	const VkResult result = vkQueueSubmit(queue, 1, &a_submitInfo, a_fence);
	interopDevice->ReleaseSubmissionQueue();

	if (result != VK_SUCCESS)
		logger::error("[FSR-RR] vkQueueSubmit failed ({})", static_cast<int>(result));
}

void FSRRRDenoiser::Denoise(
	ID3D11ShaderResourceView* a_color,
	ID3D11ShaderResourceView* a_diffuseAlbedo,
	ID3D11ShaderResourceView* a_specularAlbedo,
	ID3D11ShaderResourceView* a_normal,
	ID3D11ShaderResourceView* a_motionVectors,
	ID3D11ShaderResourceView* a_depth,
	ID3D11ShaderResourceView* a_hitDist,
	ID3D11UnorderedAccessView* a_output,
	ID3D11UnorderedAccessView* a_outputMotion,
	uint32_t a_width, uint32_t a_height)
{
	if (!IsAvailable() || !a_color || !a_normal || !a_motionVectors || !a_depth || !a_output)
		return;

	auto& self = *impl;
	if (!self.CreateResources(a_width, a_height))
		return;

	auto* context = globals::d3d::context;
	if (!context)
		return;

	const uint32_t frame = self.AcquireFrame();

	// Update shader constant buffer parameters
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(self.paramsCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto* p = reinterpret_cast<FSRRRParams*>(mapped.pData);
			p->width = a_width;
			p->height = a_height;
			p->colorExponent = self.colorDecodeExponent;
			p->padding = 0.0f;

			const auto cam = Util::GetCameraData();
			p->cameraData[0] = cam.x;
			p->cameraData[1] = cam.y;
			p->cameraData[2] = cam.z;
			p->cameraData[3] = cam.w;

			context->Unmap(self.paramsCB.get(), 0);
		}
	}

	const uint32_t dispatchX = (a_width + 7) / 8;
	const uint32_t dispatchY = (a_height + 7) / 8;

	// ---- Step 1: D3D11 Prepare pass (packs planar NCHW float tensor) ----
	{
		ID3D11ShaderResourceView* srvs[7] = {
			a_color,
			a_diffuseAlbedo ? a_diffuseAlbedo : a_color,
			a_specularAlbedo ? a_specularAlbedo : (a_diffuseAlbedo ? a_diffuseAlbedo : a_color),
			a_normal,
			a_motionVectors,
			a_depth,
			a_hitDist
		};
		ID3D11UnorderedAccessView* uavs[1] = { self.inputTensorUAV.get() };
		ID3D11Buffer* cbs[1] = { self.paramsCB.get() };

		context->CSSetShader(self.prepareCS.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 7, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->Dispatch(dispatchX, dispatchY, 1);

		ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
		ID3D11ShaderResourceView* nullSrvs[7] = {};
		context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
		context->CSSetShaderResources(0, 7, nullSrvs);
	}

	// ---- Step 2: GPU Vulkan copy from D3D11 to host-visible input staging ----
	if (self.inputVk != VK_NULL_HANDLE && self.outputVk != VK_NULL_HANDLE && self.inputMapped && self.outputMapped) {
		self.RecordCopyIn(frame);

		VkSubmitInfo submitCopyIn{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitCopyIn.commandBufferCount = 1;
		submitCopyIn.pCommandBuffers = &self.copyInCmd[frame];

		vkResetFences(self.device, 1, &self.frameFences[frame]);
		self.Submit(submitCopyIn, self.frameFences[frame]);
		vkWaitForFences(self.device, 1, &self.frameFences[frame], VK_TRUE, UINT64_MAX);

		// ---- Step 3: Neural Network Execution on Tensor Cores (ONNX Runtime / DirectML) ----
		if (self.ortReady && self.ortSession) {
			std::array<int64_t, 4> inputShape = { 1, 16, static_cast<int64_t>(a_height), static_cast<int64_t>(a_width) };
			std::array<int64_t, 4> outputShape = { 1, 16, static_cast<int64_t>(a_height), static_cast<int64_t>(a_width) };

			const char* inputNames[] = { "input_features" };
			const char* outputNames[] = { "denoised_features" };

			float* inData = reinterpret_cast<float*>(self.inputMapped);
			float* outData = reinterpret_cast<float*>(self.outputMapped);

			Ort::Value inTensor = Ort::Value::CreateTensor<float>(
				*self.ortMemoryInfo,
				inData,
				a_width * a_height * 16,
				inputShape.data(),
				inputShape.size());

			Ort::Value outTensor = Ort::Value::CreateTensor<float>(
				*self.ortMemoryInfo,
				outData,
				a_width * a_height * 16,
				outputShape.data(),
				outputShape.size());

			try {
				self.ortSession->Run(
					Ort::RunOptions{ nullptr },
					inputNames, &inTensor, 1,
					outputNames, &outTensor, 1);
			} catch (const std::exception& e) {
				logger::error("[FSR-RR] ONNX Runtime DirectML Run failed: {}", e.what());
			}
		}

		// ---- Step 4: GPU Vulkan copy out to D3D11 output tensor ----
		self.RecordCopyOut(frame);

		VkSubmitInfo submitCopyOut{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitCopyOut.commandBufferCount = 1;
		submitCopyOut.pCommandBuffers = &self.copyOutCmd[frame];

		vkResetFences(self.device, 1, &self.frameFences[frame]);
		self.Submit(submitCopyOut, self.frameFences[frame]);
		vkWaitForFences(self.device, 1, &self.frameFences[frame], VK_TRUE, UINT64_MAX);
	} else {
		// Fallback direct buffer copy if staging unavailable
		context->CopyResource(self.outputTensorD3D.get(), self.inputTensorD3D.get());
	}

	// ---- Step 5: D3D11 Re-modulation, Gamma encode, and Composite pass ----
	{
		ID3D11ShaderResourceView* srvs[5] = {
			self.outputTensorSRV.get(),
			a_color,
			a_diffuseAlbedo ? a_diffuseAlbedo : a_color,
			a_specularAlbedo ? a_specularAlbedo : (a_diffuseAlbedo ? a_diffuseAlbedo : a_color),
			a_motionVectors
		};

		ID3D11UnorderedAccessView* uavs[2] = {
			a_output,
			a_outputMotion
		};

		ID3D11Buffer* cbs[1] = { self.paramsCB.get() };

		context->CSSetShader(self.compositeCS.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 5, srvs);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		context->Dispatch(dispatchX, dispatchY, 1);

		ID3D11UnorderedAccessView* nullUavs[2] = {};
		ID3D11ShaderResourceView* nullSrvs[5] = {};
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
		context->CSSetShaderResources(0, 5, nullSrvs);
	}
}
