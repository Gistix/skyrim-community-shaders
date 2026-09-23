#include "OIDNDenoiser.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201 4458 4100)
#endif
#include <OpenImageDenoise/oidn.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "Buffer.h"
#include "DxvkLoader.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <cstring>
#include <d3d11.h>
#include <filesystem>
#include <stdexcept>

namespace
{
	struct alignas(16) OIDNParams
	{
		uint32_t width;
		uint32_t height;
		float colorExponent;
		float depthThreshold;

		uint32_t temporalEnabled;
		float historyWeight;
		uint32_t historyValid;
		float maxAccumulationFrames;

		float pad[4];

		float cameraData[4];
	};
	static_assert(sizeof(OIDNParams) == 64);

	constexpr uint32_t kFramesInFlight = 3;
	constexpr const wchar_t* kPrepareShaderPath = L"Data\\Shaders\\Raytracing\\OIDNPrepareCS.hlsl";
	constexpr const wchar_t* kCompositeShaderPath = L"Data\\Shaders\\Raytracing\\OIDNCompositeCS.hlsl";

	static oidn::Quality ToOidnQuality(OIDNDenoiser::Quality a_quality)
	{
		switch (a_quality) {
		case OIDNDenoiser::Quality::Fast:
			return oidn::Quality::Fast;
		case OIDNDenoiser::Quality::High:
			return oidn::Quality::High;
		default:
			return oidn::Quality::Balanced;
		}
	}
}

struct OIDNDenoiser::Impl
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

	// OIDN
	oidn::DeviceRef oidnDevice;
	oidn::FilterRef oidnFilter;
	oidn::BufferRef oidnColor;
	oidn::BufferRef oidnAlbedo;
	oidn::BufferRef oidnNormal;
	oidn::BufferRef oidnOutput;
	oidn::SemaphoreRef oidnSemVkToOidn;
	oidn::SemaphoreRef oidnSemOidnToVk;

	// OIDN-owned external Vulkan buffers.
	VkBuffer colorVk = VK_NULL_HANDLE;
	VkDeviceMemory colorMem = VK_NULL_HANDLE;
	VkBuffer albedoVk = VK_NULL_HANDLE;
	VkDeviceMemory albedoMem = VK_NULL_HANDLE;
	VkBuffer normalVk = VK_NULL_HANDLE;
	VkDeviceMemory normalMem = VK_NULL_HANDLE;
	VkBuffer outputVk = VK_NULL_HANDLE;
	VkDeviceMemory outputMem = VK_NULL_HANDLE;

	// Cross-API timeline semaphores.
	VkSemaphore vkSemVkToOidn = VK_NULL_HANDLE;
	VkSemaphore vkSemOidnToVk = VK_NULL_HANDLE;
	uint64_t timelineValue = 0;
	bool semaphoresReady = false;

	// Command ring.
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer copyInCmd[kFramesInFlight]{};
	VkCommandBuffer copyOutCmd[kFramesInFlight]{};
	VkFence frameFences[kFramesInFlight]{};
	uint32_t frameIndex = 0;

	// D3D11 side resources.
	winrt::com_ptr<ID3D11Buffer> colorD3D;
	winrt::com_ptr<ID3D11Buffer> albedoD3D;
	winrt::com_ptr<ID3D11Buffer> normalD3D;
	winrt::com_ptr<ID3D11Buffer> outputD3D;
	winrt::com_ptr<ID3D11UnorderedAccessView> colorUAV;
	winrt::com_ptr<ID3D11UnorderedAccessView> albedoUAV;
	winrt::com_ptr<ID3D11UnorderedAccessView> normalUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> outputSRV;
	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS;
	winrt::com_ptr<ID3D11Buffer> paramsCB;

	// Cached underlying Vulkan buffers of the D3D11 buffers.
	VkBuffer colorSrcVk = VK_NULL_HANDLE;
	VkDeviceSize colorSrcOffset = 0;
	VkBuffer albedoSrcVk = VK_NULL_HANDLE;
	VkDeviceSize albedoSrcOffset = 0;
	VkBuffer normalSrcVk = VK_NULL_HANDLE;
	VkDeviceSize normalSrcOffset = 0;
	VkBuffer outputDstVk = VK_NULL_HANDLE;
	VkDeviceSize outputDstOffset = 0;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t byteSize = 0;
	bool resourcesReady = false;

	Quality quality = Quality::Balanced;
	bool cleanAux = true;
	int memoryLimitMB = 1024;
	float colorDecodeExponent = 2.2f;

	TemporalSettings temporalSettings;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> pointSampler;

	winrt::com_ptr<ID3D11Texture2D> historyColor[2];
	winrt::com_ptr<ID3D11ShaderResourceView> historyColorSRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> historyColorUAV[2];
	winrt::com_ptr<ID3D11Texture2D> historyDepth[2];
	winrt::com_ptr<ID3D11ShaderResourceView> historyDepthSRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> historyDepthUAV[2];
	uint32_t historyReadIndex = 0;
	uint32_t historyWriteIndex = 1;
	bool historyValid = false;

	bool initialized = false;

	void DestroyResources();
	bool CreateResources(uint32_t a_width, uint32_t a_height);

	uint32_t FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const;
	bool CreateExternalBuffer(uint32_t a_size, VkBuffer& a_buffer, VkDeviceMemory& a_memory);
	void DestroyExternalBuffer(VkBuffer& a_buffer, VkDeviceMemory& a_memory);
	bool CreateSemaphores();
	void DestroySemaphores();
	void ResolveD3DBuffer(ID3D11Buffer* a_resource, VkBuffer& a_outBuffer, VkDeviceSize& a_outOffset);

	uint32_t AcquireFrame();
	void RecordCopyIn(uint32_t a_frame);
	void RecordCopyOut(uint32_t a_frame);
	void Submit(const VkSubmitInfo& a_submitInfo, VkFence a_fence);
};

OIDNDenoiser::OIDNDenoiser() :
	impl(std::make_unique<Impl>())
{}

OIDNDenoiser::~OIDNDenoiser()
{
	Shutdown();
}

bool OIDNDenoiser::IsAvailable() const
{
	return impl && impl->initialized;
}

void OIDNDenoiser::SetSettings(Quality a_quality, bool a_cleanAux, int a_memoryLimitMB)
{
	if (!impl)
		return;

	impl->quality = a_quality;
	impl->cleanAux = a_cleanAux;
	impl->memoryLimitMB = a_memoryLimitMB;

	if (impl->oidnFilter) {
		try {
			impl->oidnFilter.set("quality", ToOidnQuality(a_quality));
			impl->oidnFilter.set("cleanAux", a_cleanAux);
			impl->oidnFilter.set("maxMemoryMB", a_memoryLimitMB);
			impl->oidnFilter.commit();
		} catch (const std::exception& e) {
			logger::error("[OIDN] Failed to apply filter settings: {}", e.what());
		}
	}
}

void OIDNDenoiser::SetTemporalSettings(const TemporalSettings& a_settings)
{
	if (impl)
		impl->temporalSettings = a_settings;
}

void OIDNDenoiser::ResetHistory()
{
	if (impl)
		impl->historyValid = false;
}

void OIDNDenoiser::SetColorDecodeExponent(float a_exponent)
{
	if (impl)
		impl->colorDecodeExponent = a_exponent;
}

uint32_t OIDNDenoiser::Impl::FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const
{
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		if ((a_typeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & a_properties) == a_properties)
			return i;
	}
	return UINT32_MAX;
}

bool OIDNDenoiser::Impl::CreateExternalBuffer(uint32_t a_size, VkBuffer& a_buffer, VkDeviceMemory& a_memory)
{
	if (!getMemoryWin32Handle)
		return false;

	VkExternalMemoryBufferCreateInfo externalInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
	externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.pNext = &externalInfo;
	bufferInfo.size = a_size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(device, &bufferInfo, nullptr, &a_buffer) != VK_SUCCESS) {
		logger::error("[OIDN] vkCreateBuffer (external) failed");
		return false;
	}

	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(device, a_buffer, &memoryRequirements);

	const uint32_t memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memoryTypeIndex == UINT32_MAX) {
		logger::error("[OIDN] No device-local memory type for external buffer");
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_buffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	dedicatedInfo.buffer = a_buffer;

	VkExportMemoryAllocateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
	exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	dedicatedInfo.pNext = &exportInfo;

	VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocateInfo.pNext = &dedicatedInfo;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = memoryTypeIndex;

	if (vkAllocateMemory(device, &allocateInfo, nullptr, &a_memory) != VK_SUCCESS) {
		logger::error("[OIDN] vkAllocateMemory (external) failed");
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_buffer = VK_NULL_HANDLE;
		return false;
	}

	if (vkBindBufferMemory(device, a_buffer, a_memory, 0) != VK_SUCCESS) {
		logger::error("[OIDN] vkBindBufferMemory (external) failed");
		vkFreeMemory(device, a_memory, nullptr);
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_memory = VK_NULL_HANDLE;
		a_buffer = VK_NULL_HANDLE;
		return false;
	}

	return true;
}

void OIDNDenoiser::Impl::DestroyExternalBuffer(VkBuffer& a_buffer, VkDeviceMemory& a_memory)
{
	if (a_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, a_buffer, nullptr);
		a_buffer = VK_NULL_HANDLE;
	}
	if (a_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, a_memory, nullptr);
		a_memory = VK_NULL_HANDLE;
	}
}

bool OIDNDenoiser::Impl::CreateSemaphores()
{
	if (!getSemaphoreWin32Handle)
		return false;

	VkExportSemaphoreCreateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
	exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	typeInfo.pNext = &exportInfo;
	typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	typeInfo.initialValue = 0;

	VkSemaphoreCreateInfo createInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	createInfo.pNext = &typeInfo;

	if (vkCreateSemaphore(device, &createInfo, nullptr, &vkSemVkToOidn) != VK_SUCCESS ||
		vkCreateSemaphore(device, &createInfo, nullptr, &vkSemOidnToVk) != VK_SUCCESS) {
		logger::warn("[OIDN] Failed to create exportable timeline semaphores");
		DestroySemaphores();
		return false;
	}

	VkSemaphoreGetWin32HandleInfoKHR handleInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
	handleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	HANDLE handleVkToOidn = nullptr;
	handleInfo.semaphore = vkSemVkToOidn;
	const VkResult result1 = getSemaphoreWin32Handle(device, &handleInfo, &handleVkToOidn);

	HANDLE handleOidnToVk = nullptr;
	handleInfo.semaphore = vkSemOidnToVk;
	const VkResult result2 = getSemaphoreWin32Handle(device, &handleInfo, &handleOidnToVk);

	if (result1 != VK_SUCCESS || result2 != VK_SUCCESS || !handleVkToOidn || !handleOidnToVk) {
		logger::warn("[OIDN] Failed to export timeline semaphore handles ({}, {})",
			static_cast<int>(result1), static_cast<int>(result2));
		if (handleVkToOidn)
			CloseHandle(handleVkToOidn);
		if (handleOidnToVk)
			CloseHandle(handleOidnToVk);
		DestroySemaphores();
		return false;
	}

	try {
		oidnSemVkToOidn = oidnDevice.newSemaphore(
			oidn::ExternalSemaphoreTypeFlags(oidn::ExternalSemaphoreTypeFlag::TimelineSemaphoreWin32),
			handleVkToOidn, nullptr);
		oidnSemOidnToVk = oidnDevice.newSemaphore(
			oidn::ExternalSemaphoreTypeFlags(oidn::ExternalSemaphoreTypeFlag::TimelineSemaphoreWin32),
			handleOidnToVk, nullptr);
	} catch (const std::exception& e) {
		logger::warn("[OIDN] Failed to import timeline semaphores: {}", e.what());
		CloseHandle(handleVkToOidn);
		CloseHandle(handleOidnToVk);
		DestroySemaphores();
		return false;
	}

	CloseHandle(handleVkToOidn);
	CloseHandle(handleOidnToVk);
	timelineValue = 0;
	semaphoresReady = true;
	logger::info("[OIDN] External timeline semaphores created (GPU fenced, zero CPU wait)");
	return true;
}

void OIDNDenoiser::Impl::DestroySemaphores()
{
	oidnSemVkToOidn = nullptr;
	oidnSemOidnToVk = nullptr;
	semaphoresReady = false;

	if (device != VK_NULL_HANDLE) {
		if (vkSemVkToOidn != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, vkSemVkToOidn, nullptr);
			vkSemVkToOidn = VK_NULL_HANDLE;
		}
		if (vkSemOidnToVk != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, vkSemOidnToVk, nullptr);
			vkSemOidnToVk = VK_NULL_HANDLE;
		}
	}
}

bool OIDNDenoiser::Initialize()
{
	auto& self = *impl;

	if (self.initialized)
		return true;

	self.dxvk = DXVKInterop::GetSingleton();
	if (!self.dxvk || !self.dxvk->IsAvailable()) {
		logger::warn("[OIDN] DXVK Vulkan interop unavailable; Intel OIDN disabled");
		return false;
	}

	self.instance = self.dxvk->GetInstance();
	self.physicalDevice = self.dxvk->GetPhysicalDevice();
	self.device = self.dxvk->GetDevice();
	self.queue = self.dxvk->GetQueue();
	self.queueFamilyIndex = self.dxvk->GetQueueFamilyIndex();
	self.interopDevice = self.dxvk->GetInteropDevice();

	if (!self.device || !self.queue || !self.interopDevice) {
		logger::warn("[OIDN] Missing Vulkan device/queue; Intel OIDN disabled");
		return false;
	}

	const auto getDeviceProcAddr = self.dxvk->GetDeviceProcAddr();
	if (!getDeviceProcAddr) {
		logger::warn("[OIDN] vkGetDeviceProcAddr unavailable; Intel OIDN disabled");
		return false;
	}

	self.getMemoryWin32Handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
		getDeviceProcAddr(self.device, "vkGetMemoryWin32HandleKHR"));
	self.getSemaphoreWin32Handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
		getDeviceProcAddr(self.device, "vkGetSemaphoreWin32HandleKHR"));

	// Load the OIDN runtime from the plugin bin directory (mirrors DXVK/Streamline loading).
	// Every module is validated up front: OpenImageDenoise is delay-loaded, and a failed
	// delay-load raises an SEH exception that the std::exception handlers below cannot
	// catch, so a missing/broken runtime must be detected here to avoid crashing the game.
	// Successfully loaded modules are intentionally kept loaded for process lifetime.
	const auto runtimeDir = DxvkLoader::GetRuntimeDir();
	if (runtimeDir.empty()) {
		logger::warn("[OIDN] Could not resolve plugin runtime directory; Intel OIDN disabled");
		return false;
	}
	SetDllDirectoryW(runtimeDir.c_str());

	const auto loadRuntimeModule = [&runtimeDir](const wchar_t* a_name) -> HMODULE {
		const std::filesystem::path path = runtimeDir / a_name;
		HMODULE handle = LoadLibraryW(path.c_str());
		if (!handle)
			logger::warn("[OIDN] LoadLibrary failed for {} (error {})", path.string(), ::GetLastError());
		return handle;
	};

	const HMODULE coreLib = loadRuntimeModule(L"OpenImageDenoise_core.dll");
	const HMODULE apiLib = loadRuntimeModule(L"OpenImageDenoise.dll");
	const HMODULE cudaLib = loadRuntimeModule(L"OpenImageDenoise_device_cuda.dll");
	if (!coreLib || !apiLib) {
		logger::warn("[OIDN] OIDN runtime unavailable; Intel OIDN disabled");
		return false;
	}
	if (!cudaLib)
		logger::info("[OIDN] CUDA backend module not found; will fall back to SYCL/HIP/default devices");
	// Probe the delay-loaded entry point while the module is known loaded.
	if (!GetProcAddress(apiLib, "oidnNewDevice")) {
		logger::warn("[OIDN] oidnNewDevice export missing (error {})", ::GetLastError());
		return false;
	}

	try {
		self.oidnDevice = oidn::newDevice(oidn::DeviceType::CUDA);
	} catch (...) {
		self.oidnDevice = nullptr;
	}
	if (!self.oidnDevice) {
		try {
			self.oidnDevice = oidn::newDevice(oidn::DeviceType::SYCL);
		} catch (...) {
			self.oidnDevice = nullptr;
		}
	}
	if (!self.oidnDevice) {
		try {
			self.oidnDevice = oidn::newDevice(oidn::DeviceType::HIP);
		} catch (...) {
			self.oidnDevice = nullptr;
		}
	}
	if (!self.oidnDevice) {
		try {
			self.oidnDevice = oidn::newDevice(oidn::DeviceType::Default);
		} catch (...) {
			self.oidnDevice = nullptr;
		}
	}

	if (!self.oidnDevice) {
		logger::error("[OIDN] Failed to initialize a GPU OIDN device; Intel OIDN disabled");
		return false;
	}

	try {
		self.oidnDevice.setErrorFunction([]([[maybe_unused]] void* userPtr, oidn::Error error, const char* message) {
			logger::error("[OIDN] error ({}): {}", static_cast<int>(error), message ? message : "<unknown>");
		},
			nullptr);
		self.oidnDevice.commit();
	} catch (const std::exception& e) {
		logger::error("[OIDN] Failed to commit OIDN device: {}", e.what());
		self.oidnDevice = nullptr;
		return false;
	}

	// D3D11 shaders.
	self.prepareCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(kPrepareShaderPath, {}, "cs_5_0")));
	self.compositeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(kCompositeShaderPath, {}, "cs_5_0")));
	if (!self.prepareCS || !self.compositeCS) {
		logger::error("[OIDN] Failed to compile OIDN compute shaders; Intel OIDN disabled");
		return false;
	}

	auto* device = globals::d3d::device;
	if (!device) {
		logger::error("[OIDN] No D3D11 device; Intel OIDN disabled");
		return false;
	}

	// Dynamic constant buffer shared by both passes.
	D3D11_BUFFER_DESC cbDesc = ConstantBufferDesc<OIDNParams>();
	if (FAILED(device->CreateBuffer(&cbDesc, nullptr, self.paramsCB.put()))) {
		logger::error("[OIDN] Failed to create OIDN parameter buffer");
		return false;
	}

	D3D11_SAMPLER_DESC samplerDesc = {
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
		.ComparisonFunc = D3D11_COMPARISON_NEVER,
		.MinLOD = 0,
		.MaxLOD = D3D11_FLOAT32_MAX
	};
	if (FAILED(device->CreateSamplerState(&samplerDesc, self.linearSampler.put()))) {
		logger::error("[OIDN] Failed to create linear sampler");
		return false;
	}

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	if (FAILED(device->CreateSamplerState(&samplerDesc, self.pointSampler.put()))) {
		logger::error("[OIDN] Failed to create point sampler");
		return false;
	}

	// Command pool + command buffers + fences.
	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = self.queueFamilyIndex;
	if (vkCreateCommandPool(self.device, &poolInfo, nullptr, &self.commandPool) != VK_SUCCESS) {
		logger::error("[OIDN] Failed to create Vulkan command pool");
		return false;
	}

	VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocateInfo.commandPool = self.commandPool;
	allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocateInfo.commandBufferCount = kFramesInFlight;
	if (vkAllocateCommandBuffers(self.device, &allocateInfo, self.copyInCmd) != VK_SUCCESS ||
		vkAllocateCommandBuffers(self.device, &allocateInfo, self.copyOutCmd) != VK_SUCCESS) {
		logger::error("[OIDN] Failed to allocate Vulkan command buffers");
		return false;
	}

	VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < kFramesInFlight; ++i) {
		if (vkCreateFence(self.device, &fenceInfo, nullptr, &self.frameFences[i]) != VK_SUCCESS) {
			logger::error("[OIDN] Failed to create Vulkan fences");
			return false;
		}
	}

	self.initialized = true;
	logger::info("[OIDN] Intel Open Image Denoise GPU denoiser initialized");
	return true;
}

void OIDNDenoiser::Shutdown()
{
	if (!impl)
		return;

	auto& self = *impl;
	if (!self.initialized) {
		self.initialized = false;
		return;
	}

	self.DestroyResources();

	self.oidnFilter = nullptr;

	self.DestroySemaphores();

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

	self.oidnDevice = nullptr;
	self.prepareCS = nullptr;
	self.compositeCS = nullptr;
	self.paramsCB = nullptr;
	self.linearSampler = nullptr;
	self.pointSampler = nullptr;
	self.initialized = false;
}

void OIDNDenoiser::Impl::ResolveD3DBuffer(ID3D11Buffer* a_resource, VkBuffer& a_outBuffer, VkDeviceSize& a_outOffset)
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

bool OIDNDenoiser::Impl::CreateResources(uint32_t a_width, uint32_t a_height)
{
	if (a_width == 0 || a_height == 0)
		return false;

	if (resourcesReady && a_width == width && a_height == height)
		return true;

	DestroyResources();

	auto* d3dDevice = globals::d3d::device;
	if (!d3dDevice)
		return false;

	width = a_width;
	height = a_height;
	const uint32_t pixelCount = width * height;
	byteSize = pixelCount * 8;

	// D3D11 structured buffers (packed fp16, stride 8).
	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = 8;
	bufferDesc.ByteWidth = byteSize;

	if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, colorD3D.put())) ||
		FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, albedoD3D.put())) ||
		FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, normalD3D.put()))) {
		logger::error("[OIDN] Failed to create input buffers");
		DestroyResources();
		return false;
	}

	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, outputD3D.put()))) {
		logger::error("[OIDN] Failed to create output buffer");
		DestroyResources();
		return false;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = pixelCount;
	if (FAILED(d3dDevice->CreateUnorderedAccessView(colorD3D.get(), &uavDesc, colorUAV.put())) ||
		FAILED(d3dDevice->CreateUnorderedAccessView(albedoD3D.get(), &uavDesc, albedoUAV.put())) ||
		FAILED(d3dDevice->CreateUnorderedAccessView(normalD3D.get(), &uavDesc, normalUAV.put()))) {
		logger::error("[OIDN] Failed to create input UAVs");
		DestroyResources();
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = pixelCount;
	if (FAILED(d3dDevice->CreateShaderResourceView(outputD3D.get(), &srvDesc, outputSRV.put()))) {
		logger::error("[OIDN] Failed to create output SRV");
		DestroyResources();
		return false;
	}

	// Cache the underlying Vulkan buffers while they are pinned by the interop lock.
	ResolveD3DBuffer(colorD3D.get(), colorSrcVk, colorSrcOffset);
	ResolveD3DBuffer(albedoD3D.get(), albedoSrcVk, albedoSrcOffset);
	ResolveD3DBuffer(normalD3D.get(), normalSrcVk, normalSrcOffset);
	ResolveD3DBuffer(outputD3D.get(), outputDstVk, outputDstOffset);

	if (colorSrcVk == VK_NULL_HANDLE || albedoSrcVk == VK_NULL_HANDLE ||
		normalSrcVk == VK_NULL_HANDLE || outputDstVk == VK_NULL_HANDLE) {
		logger::error("[OIDN] Failed to resolve D3D11 buffers to Vulkan buffers");
		DestroyResources();
		return false;
	}

	// OIDN-owned external Vulkan buffers + OIDN buffer imports.
	if (!CreateExternalBuffer(byteSize, colorVk, colorMem) ||
		!CreateExternalBuffer(byteSize, albedoVk, albedoMem) ||
		!CreateExternalBuffer(byteSize, normalVk, normalMem) ||
		!CreateExternalBuffer(byteSize, outputVk, outputMem)) {
		DestroyResources();
		return false;
	}

	try {
		auto externalFlags = oidn::ExternalMemoryTypeFlags(oidn::ExternalMemoryTypeFlag::OpaqueWin32) |
		                     oidn::ExternalMemoryTypeFlag::Dedicated;

		VkMemoryGetWin32HandleInfoKHR getHandleInfo{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
		getHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

		auto importBuffer = [&](VkDeviceMemory a_memory) -> oidn::BufferRef {
			HANDLE handle = nullptr;
			getHandleInfo.memory = a_memory;
			if (getMemoryWin32Handle(device, &getHandleInfo, &handle) != VK_SUCCESS || !handle)
				throw std::runtime_error("vkGetMemoryWin32HandleKHR failed");
			auto buffer = oidnDevice.newBuffer(externalFlags, handle, nullptr, byteSize);
			CloseHandle(handle);
			return buffer;
		};

		oidnColor = importBuffer(colorMem);
		oidnAlbedo = importBuffer(albedoMem);
		oidnNormal = importBuffer(normalMem);
		oidnOutput = importBuffer(outputMem);

		oidnFilter = oidnDevice.newFilter("RT");
		const size_t pixelStride = 8;
		const size_t rowStride = static_cast<size_t>(width) * pixelStride;
		oidnFilter.setImage("color", oidnColor, oidn::Format::Half3, width, height, 0, pixelStride, rowStride);
		oidnFilter.setImage("albedo", oidnAlbedo, oidn::Format::Half3, width, height, 0, pixelStride, rowStride);
		oidnFilter.setImage("normal", oidnNormal, oidn::Format::Half3, width, height, 0, pixelStride, rowStride);
		oidnFilter.setImage("output", oidnOutput, oidn::Format::Half3, width, height, 0, pixelStride, rowStride);
		oidnFilter.set("hdr", true);
		oidnFilter.set("cleanAux", cleanAux);
		oidnFilter.set("quality", ToOidnQuality(quality));
		oidnFilter.set("maxMemoryMB", memoryLimitMB);
		oidnFilter.commit();
	} catch (const std::exception& e) {
		logger::error("[OIDN] Failed to bind shared buffers: {}", e.what());
		DestroyResources();
		return false;
	}

	// History color textures (RGBA16_FLOAT)
	D3D11_TEXTURE2D_DESC historyColorDesc{};
	historyColorDesc.Width = width;
	historyColorDesc.Height = height;
	historyColorDesc.MipLevels = 1;
	historyColorDesc.ArraySize = 1;
	historyColorDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	historyColorDesc.SampleDesc.Count = 1;
	historyColorDesc.SampleDesc.Quality = 0;
	historyColorDesc.Usage = D3D11_USAGE_DEFAULT;
	historyColorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDescColor{};
	srvDescColor.Format = historyColorDesc.Format;
	srvDescColor.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDescColor.Texture2D.MostDetailedMip = 0;
	srvDescColor.Texture2D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescColor{};
	uavDescColor.Format = historyColorDesc.Format;
	uavDescColor.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDescColor.Texture2D.MipSlice = 0;

	// History depth textures (R32_FLOAT)
	D3D11_TEXTURE2D_DESC historyDepthDesc = historyColorDesc;
	historyDepthDesc.Format = DXGI_FORMAT_R32_FLOAT;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDescDepth{};
	srvDescDepth.Format = historyDepthDesc.Format;
	srvDescDepth.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDescDepth.Texture2D.MostDetailedMip = 0;
	srvDescDepth.Texture2D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescDepth{};
	uavDescDepth.Format = historyDepthDesc.Format;
	uavDescDepth.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDescDepth.Texture2D.MipSlice = 0;

	for (uint32_t i = 0; i < 2; ++i) {
		if (FAILED(d3dDevice->CreateTexture2D(&historyColorDesc, nullptr, historyColor[i].put())) ||
			FAILED(d3dDevice->CreateShaderResourceView(historyColor[i].get(), &srvDescColor, historyColorSRV[i].put())) ||
			FAILED(d3dDevice->CreateUnorderedAccessView(historyColor[i].get(), &uavDescColor, historyColorUAV[i].put()))) {
			logger::error("[OIDN] Failed to create history color resources");
			DestroyResources();
			return false;
		}

		if (FAILED(d3dDevice->CreateTexture2D(&historyDepthDesc, nullptr, historyDepth[i].put())) ||
			FAILED(d3dDevice->CreateShaderResourceView(historyDepth[i].get(), &srvDescDepth, historyDepthSRV[i].put())) ||
			FAILED(d3dDevice->CreateUnorderedAccessView(historyDepth[i].get(), &uavDescDepth, historyDepthUAV[i].put()))) {
			logger::error("[OIDN] Failed to create history depth resources");
			DestroyResources();
			return false;
		}
	}

	historyReadIndex = 0;
	historyWriteIndex = 1;
	historyValid = false;

	if (!semaphoresReady)
		CreateSemaphores();

	resourcesReady = true;
	logger::info("[OIDN] Resources created at {}x{}", width, height);
	return true;
}

void OIDNDenoiser::Impl::DestroyResources()
{
	oidnFilter = nullptr;
	oidnColor = nullptr;
	oidnAlbedo = nullptr;
	oidnNormal = nullptr;
	oidnOutput = nullptr;

	if (device != VK_NULL_HANDLE) {
		DestroyExternalBuffer(colorVk, colorMem);
		DestroyExternalBuffer(albedoVk, albedoMem);
		DestroyExternalBuffer(normalVk, normalMem);
		DestroyExternalBuffer(outputVk, outputMem);
	}

	colorSrcVk = VK_NULL_HANDLE;
	albedoSrcVk = VK_NULL_HANDLE;
	normalSrcVk = VK_NULL_HANDLE;
	outputDstVk = VK_NULL_HANDLE;

	colorD3D = nullptr;
	albedoD3D = nullptr;
	normalD3D = nullptr;
	outputD3D = nullptr;
	colorUAV = nullptr;
	albedoUAV = nullptr;
	normalUAV = nullptr;
	outputSRV = nullptr;

	for (uint32_t i = 0; i < 2; ++i) {
		historyColor[i] = nullptr;
		historyColorSRV[i] = nullptr;
		historyColorUAV[i] = nullptr;
		historyDepth[i] = nullptr;
		historyDepthSRV[i] = nullptr;
		historyDepthUAV[i] = nullptr;
	}
	historyValid = false;

	width = 0;
	height = 0;
	byteSize = 0;
	resourcesReady = false;
}

uint32_t OIDNDenoiser::Impl::AcquireFrame()
{
	const uint32_t frame = frameIndex;
	frameIndex = (frameIndex + 1) % kFramesInFlight;

	const VkResult status = vkGetFenceStatus(device, frameFences[frame]);
	if (status == VK_NOT_READY)
		vkWaitForFences(device, 1, &frameFences[frame], VK_TRUE, UINT64_MAX);

	return frame;
}

void OIDNDenoiser::Impl::Submit(const VkSubmitInfo& a_submitInfo, VkFence a_fence)
{
	if (!interopDevice)
		return;

	interopDevice->FlushRenderingCommands();
	interopDevice->LockSubmissionQueue();
	const VkResult result = vkQueueSubmit(queue, 1, &a_submitInfo, a_fence);
	interopDevice->ReleaseSubmissionQueue();

	if (result != VK_SUCCESS)
		logger::error("[OIDN] vkQueueSubmit failed ({})", static_cast<int>(result));
}

void OIDNDenoiser::Impl::RecordCopyIn(uint32_t a_frame)
{
	const VkCommandBuffer cb = copyInCmd[a_frame];
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &beginInfo);

	VkBufferMemoryBarrier barriers[6]{};
	VkBuffer srcBuffers[3] = { colorSrcVk, albedoSrcVk, normalSrcVk };
	VkBuffer dstBuffers[3] = { colorVk, albedoVk, normalVk };

	for (uint32_t i = 0; i < 3; ++i) {
		auto& src = barriers[i];
		src.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		src.buffer = srcBuffers[i];
		src.offset = 0;
		src.size = VK_WHOLE_SIZE;

		auto& dst = barriers[3 + i];
		dst.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		dst.srcAccessMask = 0;
		dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dst.buffer = dstBuffers[i];
		dst.offset = 0;
		dst.size = VK_WHOLE_SIZE;
	}

	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, nullptr, 6, barriers, 0, nullptr);

	VkBufferCopy copy{};
	copy.dstOffset = 0;
	copy.size = byteSize;
	copy.srcOffset = colorSrcOffset;
	vkCmdCopyBuffer(cb, colorSrcVk, colorVk, 1, &copy);
	copy.srcOffset = albedoSrcOffset;
	vkCmdCopyBuffer(cb, albedoSrcVk, albedoVk, 1, &copy);
	copy.srcOffset = normalSrcOffset;
	vkCmdCopyBuffer(cb, normalSrcVk, normalVk, 1, &copy);

	// Ensure the writes are visible to the external OIDN device.
	VkMemoryBarrier memoryBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
		1, &memoryBarrier, 0, nullptr, 0, nullptr);

	vkEndCommandBuffer(cb);
}

void OIDNDenoiser::Impl::RecordCopyOut(uint32_t a_frame)
{
	const VkCommandBuffer cb = copyOutCmd[a_frame];
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &beginInfo);

	VkBufferMemoryBarrier barriers[2]{};
	barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer = outputVk;
	barriers[0].offset = 0;
	barriers[0].size = VK_WHOLE_SIZE;

	barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barriers[1].srcAccessMask = 0;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].buffer = outputDstVk;
	barriers[1].offset = 0;
	barriers[1].size = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, nullptr, 2, barriers, 0, nullptr);

	VkBufferCopy copy{};
	copy.srcOffset = 0;
	copy.dstOffset = outputDstOffset;
	copy.size = byteSize;
	vkCmdCopyBuffer(cb, outputVk, outputDstVk, 1, &copy);

	// Make the denoised data visible to the following D3D11 composite dispatch.
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[0].buffer = outputDstVk;
	barriers[0].offset = 0;
	barriers[0].size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		0, nullptr, 1, barriers, 0, nullptr);

	vkEndCommandBuffer(cb);
}

void OIDNDenoiser::Denoise(ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_albedo,
	ID3D11ShaderResourceView* a_normal, ID3D11ShaderResourceView* a_motionVectors,
	ID3D11ShaderResourceView* a_depth, ID3D11UnorderedAccessView* a_output,
	ID3D11UnorderedAccessView* a_outputMotion,
	uint32_t a_width, uint32_t a_height)
{
	auto& self = *impl;
	if (!self.initialized || !a_color || !a_albedo || !a_normal || !a_output)
		return;

	if (!self.CreateResources(a_width, a_height))
		return;

	auto* context = globals::d3d::context;
	if (!context)
		return;

	const uint32_t frame = self.AcquireFrame();
	const bool async = self.semaphoresReady;
	const uint64_t value = async ? ++self.timelineValue : 0;

	// ---- Step 1: D3D11 prepare pass ----
	{
		OIDNParams params{};
		params.width = a_width;
		params.height = a_height;
		params.colorExponent = self.colorDecodeExponent;
		params.depthThreshold = 0.0f;
		params.temporalEnabled = 0;
		params.historyWeight = 0.0f;
		params.historyValid = 0;
		params.maxAccumulationFrames = 1.0f;
		const auto cam = Util::GetCameraData();
		params.cameraData[0] = cam.x;
		params.cameraData[1] = cam.y;
		params.cameraData[2] = cam.z;
		params.cameraData[3] = cam.w;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(self.paramsCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &params, sizeof(params));
			context->Unmap(self.paramsCB.get(), 0);
		}

		context->CSSetShader(self.prepareCS.get(), nullptr, 0);
		ID3D11Buffer* cb = self.paramsCB.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		ID3D11Buffer* perFrameBuf = *globals::game::perFrame.get();
		context->CSSetConstantBuffers(12, 1, &perFrameBuf);
		ID3D11ShaderResourceView* srvs[3] = { a_color, a_albedo, a_normal };
		context->CSSetShaderResources(0, 3, srvs);
		ID3D11UnorderedAccessView* uavs[3] = { self.colorUAV.get(), self.albedoUAV.get(), self.normalUAV.get() };
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

		context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);

		ID3D11Buffer* nullCb = nullptr;
		context->CSSetConstantBuffers(12, 1, &nullCb);
		ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, 3, nullSrvs);
		ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// ---- Step 2: bridge into OIDN via Vulkan buffer copies ----
	self.RecordCopyIn(frame);

	if (async) {
		VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
		timelineInfo.signalSemaphoreValueCount = 1;
		timelineInfo.pSignalSemaphoreValues = &value;

		VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.pNext = &timelineInfo;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &self.copyInCmd[frame];
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &self.vkSemVkToOidn;

		self.Submit(submitInfo, VK_NULL_HANDLE);

		try {
			self.oidnDevice.waitSemaphoreAsync(self.oidnSemVkToOidn, value);
			self.oidnFilter.executeAsync();
			self.oidnDevice.signalSemaphoreAsync(self.oidnSemOidnToVk, value);
		} catch (const std::exception& e) {
			logger::error("[OIDN] executeAsync failed: {}", e.what());
			return;
		}

		self.RecordCopyOut(frame);

		VkTimelineSemaphoreSubmitInfo outTimeline{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
		outTimeline.waitSemaphoreValueCount = 1;
		outTimeline.pWaitSemaphoreValues = &value;

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo outSubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		outSubmit.pNext = &outTimeline;
		outSubmit.waitSemaphoreCount = 1;
		outSubmit.pWaitSemaphores = &self.vkSemOidnToVk;
		outSubmit.pWaitDstStageMask = &waitStage;
		outSubmit.commandBufferCount = 1;
		outSubmit.pCommandBuffers = &self.copyOutCmd[frame];

		vkResetFences(self.device, 1, &self.frameFences[frame]);
		self.Submit(outSubmit, self.frameFences[frame]);
	} else {
		VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &self.copyInCmd[frame];

		vkResetFences(self.device, 1, &self.frameFences[frame]);
		self.Submit(submitInfo, self.frameFences[frame]);
		vkWaitForFences(self.device, 1, &self.frameFences[frame], VK_TRUE, UINT64_MAX);

		try {
			self.oidnFilter.execute();
		} catch (const std::exception& e) {
			logger::error("[OIDN] execute failed: {}", e.what());
			return;
		}

		self.RecordCopyOut(frame);
		VkSubmitInfo outSubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		outSubmit.commandBufferCount = 1;
		outSubmit.pCommandBuffers = &self.copyOutCmd[frame];
		vkResetFences(self.device, 1, &self.frameFences[frame]);
		self.Submit(outSubmit, self.frameFences[frame]);
		vkWaitForFences(self.device, 1, &self.frameFences[frame], VK_TRUE, UINT64_MAX);
	}

	// ---- Step 4: D3D11 composite & temporal stabilization pass ----
	{
		const bool temporalActive = self.temporalSettings.enabled && a_motionVectors && a_depth;

		OIDNParams params{};
		params.width = a_width;
		params.height = a_height;
		params.colorExponent = self.colorDecodeExponent > 0.0f ? 1.0f / self.colorDecodeExponent : 1.0f;
		params.depthThreshold = std::clamp(self.temporalSettings.depthThreshold, 0.0001f, 1.0f);

		params.temporalEnabled = temporalActive ? 1 : 0;
		params.historyWeight = std::clamp(self.temporalSettings.historyWeight, 0.0f, 1.0f);
		params.historyValid = (self.historyValid && temporalActive) ? 1 : 0;
		params.maxAccumulationFrames = static_cast<float>(std::clamp(self.temporalSettings.maxAccumulationFrames, 1, 128));

		const auto cam = Util::GetCameraData();
		params.cameraData[0] = cam.x;
		params.cameraData[1] = cam.y;
		params.cameraData[2] = cam.z;
		params.cameraData[3] = cam.w;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(self.paramsCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &params, sizeof(params));
			context->Unmap(self.paramsCB.get(), 0);
		}

		context->CSSetShader(self.compositeCS.get(), nullptr, 0);
		ID3D11Buffer* cb = self.paramsCB.get();
		context->CSSetConstantBuffers(0, 1, &cb);

		ID3D11SamplerState* samplers[2] = { self.linearSampler.get(), self.pointSampler.get() };
		context->CSSetSamplers(0, 2, samplers);

		ID3D11ShaderResourceView* srvs[6] = {
			self.outputSRV.get(),
			a_color,
			a_motionVectors,
			a_depth,
			self.historyColorSRV[self.historyReadIndex].get(),
			self.historyDepthSRV[self.historyReadIndex].get()
		};
		context->CSSetShaderResources(0, 6, srvs);

		ID3D11UnorderedAccessView* uavs[4] = {
			a_output,
			a_outputMotion,
			self.historyColorUAV[self.historyWriteIndex].get(),
			self.historyDepthUAV[self.historyWriteIndex].get()
		};
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);

		context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSrvs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, 6, nullSrvs);
		ID3D11UnorderedAccessView* nullUavs[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
		ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
		context->CSSetSamplers(0, 2, nullSamplers);
		context->CSSetShader(nullptr, nullptr, 0);

		if (temporalActive) {
			std::swap(self.historyReadIndex, self.historyWriteIndex);
			self.historyValid = true;
		}
	}
}
