#include "D3D12Interop.h"

#include "DXVKInterop.h"
#include "Globals.h"

#include <algorithm>
#include <cstring>

D3D12Interop* D3D12Interop::GetSingleton()
{
	static D3D12Interop singleton;
	return &singleton;
}

D3D12Interop::~D3D12Interop()
{
	Shutdown();
}

uint32_t D3D12Interop::FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const
{
	if (!vkPhysicalDevice)
		return UINT32_MAX;

	VkPhysicalDeviceMemoryProperties memProperties{};
	vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
		if ((a_typeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & a_properties) == a_properties) {
			return i;
		}
	}

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
		if (a_typeBits & (1 << i)) {
			return i;
		}
	}

	return UINT32_MAX;
}

bool D3D12Interop::Initialize(VkInstance a_vkInstance, VkPhysicalDevice a_vkPhysDev, VkDevice a_vkDevice)
{
	if (available)
		return true;

	if (!a_vkInstance || !a_vkPhysDev || !a_vkDevice) {
		logger::error("[D3D12Interop] Invalid Vulkan handles passed to Initialize");
		return false;
	}

	vkInstance = a_vkInstance;
	vkPhysicalDevice = a_vkPhysDev;
	vkDevice = a_vkDevice;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk) {
		logger::error("[D3D12Interop] DXVKInterop singleton unavailable");
		return false;
	}

	auto pfnGetDeviceProcAddr = dxvk->GetDeviceProcAddr();
	if (!pfnGetDeviceProcAddr) {
		logger::error("[D3D12Interop] vkGetDeviceProcAddr unavailable");
		return false;
	}

	vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		pfnGetDeviceProcAddr(vkDevice, "vkGetMemoryWin32HandlePropertiesKHR"));
	vkImportSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		pfnGetDeviceProcAddr(vkDevice, "vkImportSemaphoreWin32HandleKHR"));
	vkSignalSemaphoreKHR = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
		pfnGetDeviceProcAddr(vkDevice, "vkSignalSemaphoreKHR"));
	if (!vkSignalSemaphoreKHR) {
		vkSignalSemaphoreKHR = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
			pfnGetDeviceProcAddr(vkDevice, "vkSignalSemaphore"));
	}
	vkWaitSemaphoresKHR = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
		pfnGetDeviceProcAddr(vkDevice, "vkWaitSemaphoresKHR"));
	if (!vkWaitSemaphoresKHR) {
		vkWaitSemaphoresKHR = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
			pfnGetDeviceProcAddr(vkDevice, "vkWaitSemaphores"));
	}

	if (!vkGetMemoryWin32HandlePropertiesKHR || !vkImportSemaphoreWin32HandleKHR) {
		logger::error("[D3D12Interop] Vulkan Win32 external memory or semaphore extensions not exported by DXVK device");
		return false;
	}

	// 1. Identify adapter LUID from Vulkan device properties
	LUID targetLuid{};
	bool luidFound = false;

	auto pfnGetPhysDevProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
		dxvk->GetInstanceProcAddr()(vkInstance, "vkGetPhysicalDeviceProperties2"));
	if (pfnGetPhysDevProps2) {
		VkPhysicalDeviceIDProperties idProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
		pfnGetPhysDevProps2(vkPhysicalDevice, &props2);

		if (idProps.deviceLUIDValid) {
			std::memcpy(&targetLuid, idProps.deviceLUID, sizeof(LUID));
			luidFound = true;
		}
	}

	// 2. Locate matching DXGI Adapter
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.put()));
	if (FAILED(hr) || !dxgiFactory) {
		logger::error("[D3D12Interop] Failed to create DXGIFactory1 ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	if (luidFound) {
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, dxgiAdapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			dxgiAdapter->GetDesc1(&desc);
			if (desc.AdapterLuid.LowPart == targetLuid.LowPart && desc.AdapterLuid.HighPart == targetLuid.HighPart) {
				char descNarrow[128]{};
				WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, descNarrow, sizeof(descNarrow), nullptr, nullptr);
				logger::info("[D3D12Interop] Matched DXGI adapter: '{}' (LUID {:x}:{:x})",
					descNarrow, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
				break;
			}
			dxgiAdapter = nullptr;
		}
	}

	if (!dxgiAdapter) {
		logger::warn("[D3D12Interop] Exact LUID adapter match not found; falling back to primary adapter");
		dxgiFactory->EnumAdapters1(0, dxgiAdapter.put());
	}

	if (!dxgiAdapter) {
		logger::error("[D3D12Interop] Failed to acquire DXGI adapter");
		return false;
	}

	// 3. Create D3D12 Device
	hr = D3D12CreateDevice(dxgiAdapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put()));
	if (FAILED(hr) || !device) {
		logger::error("[D3D12Interop] D3D12CreateDevice failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 4. Create Direct Command Queue
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.NodeMask = 0;
	hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put()));
	if (FAILED(hr) || !commandQueue) {
		logger::error("[D3D12Interop] CreateCommandQueue failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 5. Create per-frame Command Allocators and Command Lists
	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
		hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandAllocators[i].put()));
		if (FAILED(hr)) {
			logger::error("[D3D12Interop] CreateCommandAllocator[{}] failed ({:#x})", i, static_cast<uint32_t>(hr));
			return false;
		}

		hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(commandLists[i].put()));
		if (FAILED(hr)) {
			logger::error("[D3D12Interop] CreateCommandList[{}] failed ({:#x})", i, static_cast<uint32_t>(hr));
			return false;
		}

		commandLists[i]->Close();
	}

	// 6. Create main shared timeline fence
	if (!CreateSharedFence(mainFence, 0)) {
		logger::error("[D3D12Interop] Failed to create main shared timeline fence");
		return false;
	}

	available = true;
	logger::info("[D3D12Interop] Successfully initialized Vulkan <-> D3D12 GPU interop system");
	return true;
}

void D3D12Interop::Shutdown()
{
	if (!available)
		return;

	DestroySharedFence(mainFence);

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
		commandLists[i] = nullptr;
		commandAllocators[i] = nullptr;
	}

	commandQueue = nullptr;
	device = nullptr;
	dxgiAdapter = nullptr;
	dxgiFactory = nullptr;

	vkDevice = VK_NULL_HANDLE;
	vkPhysicalDevice = VK_NULL_HANDLE;
	vkInstance = VK_NULL_HANDLE;

	available = false;
	logger::info("[D3D12Interop] Shutdown completed");
}

bool D3D12Interop::CreateSharedTexture2D(
	uint32_t a_width, uint32_t a_height,
	DXGI_FORMAT a_dxgiFormat, VkFormat a_vkFormat,
	D3D12_RESOURCE_FLAGS a_d3dFlags, VkImageUsageFlags a_vkUsage,
	SharedTexture& a_outTexture)
{
	if (!device || !vkDevice)
		return false;

	DestroySharedTexture(a_outTexture);

	// 1. Create committed resource in D3D12
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = a_width;
	desc.Height = a_height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = a_dxgiFormat;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = a_d3dFlags;

	D3D12_HEAP_PROPERTIES heapProps{};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	winrt::com_ptr<ID3D12Resource> d3dRes;
	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_SHARED,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(d3dRes.put()));

	if (FAILED(hr) || !d3dRes) {
		logger::error("[D3D12Interop] CreateCommittedResource for texture failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 2. Export shared NT handle
	HANDLE sharedHandle = nullptr;
	hr = device->CreateSharedHandle(d3dRes.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
	if (FAILED(hr) || !sharedHandle) {
		logger::error("[D3D12Interop] CreateSharedHandle for texture failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 3. Create Vulkan image with external memory info
	VkExternalMemoryImageCreateInfo extImageInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	extImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

	VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.pNext = &extImageInfo;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = a_vkFormat;
	imageInfo.extent = { a_width, a_height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = a_vkUsage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage vkImg = VK_NULL_HANDLE;
	if (vkCreateImage(vkDevice, &imageInfo, nullptr, &vkImg) != VK_SUCCESS) {
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkCreateImage with external memory failed");
		return false;
	}

	// 4. Query memory type bits supported for this NT handle
	VkMemoryWin32HandlePropertiesKHR handleProps{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
	if (vkGetMemoryWin32HandlePropertiesKHR(vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, sharedHandle, &handleProps) != VK_SUCCESS) {
		vkDestroyImage(vkDevice, vkImg, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkGetMemoryWin32HandlePropertiesKHR failed");
		return false;
	}

	VkMemoryRequirements memReqs{};
	vkGetImageMemoryRequirements(vkDevice, vkImg, &memReqs);

	uint32_t memTypeIndex = FindMemoryType(handleProps.memoryTypeBits & memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memTypeIndex == UINT32_MAX) {
		vkDestroyImage(vkDevice, vkImg, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] No compatible memory type for imported D3D12 texture");
		return false;
	}

	// 5. Import memory and bind to image
	VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
	importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
	importInfo.handle = sharedHandle;

	VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.pNext = &importInfo;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = memTypeIndex;

	VkDeviceMemory vkMem = VK_NULL_HANDLE;
	if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &vkMem) != VK_SUCCESS) {
		vkDestroyImage(vkDevice, vkImg, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkAllocateMemory for imported texture failed");
		return false;
	}

	vkBindImageMemory(vkDevice, vkImg, vkMem, 0);

	// 6. Create default image view
	VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = vkImg;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = a_vkFormat;
	viewInfo.subresourceRange.aspectMask = (a_vkUsage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		? VK_IMAGE_ASPECT_DEPTH_BIT
		: VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView vkView = VK_NULL_HANDLE;
	vkCreateImageView(vkDevice, &viewInfo, nullptr, &vkView);

	a_outTexture.d3dResource = std::move(d3dRes);
	a_outTexture.sharedHandle = sharedHandle;
	a_outTexture.vkImage = vkImg;
	a_outTexture.vkMemory = vkMem;
	a_outTexture.vkView = vkView;
	a_outTexture.width = a_width;
	a_outTexture.height = a_height;
	a_outTexture.dxgiFormat = a_dxgiFormat;
	a_outTexture.vkFormat = a_vkFormat;

	return true;
}

void D3D12Interop::DestroySharedTexture(SharedTexture& a_texture)
{
	if (vkDevice) {
		if (a_texture.vkView) {
			vkDestroyImageView(vkDevice, a_texture.vkView, nullptr);
			a_texture.vkView = VK_NULL_HANDLE;
		}
		if (a_texture.vkImage) {
			vkDestroyImage(vkDevice, a_texture.vkImage, nullptr);
			a_texture.vkImage = VK_NULL_HANDLE;
		}
		if (a_texture.vkMemory) {
			vkFreeMemory(vkDevice, a_texture.vkMemory, nullptr);
			a_texture.vkMemory = VK_NULL_HANDLE;
		}
	}

	if (a_texture.sharedHandle) {
		CloseHandle(a_texture.sharedHandle);
		a_texture.sharedHandle = nullptr;
	}

	a_texture.d3dResource = nullptr;
	a_texture.width = 0;
	a_texture.height = 0;
	a_texture.dxgiFormat = DXGI_FORMAT_UNKNOWN;
	a_texture.vkFormat = VK_FORMAT_UNDEFINED;
}

bool D3D12Interop::CreateSharedBuffer(
	uint64_t a_size,
	D3D12_RESOURCE_FLAGS a_d3dFlags, VkBufferUsageFlags a_vkUsage,
	SharedBuffer& a_outBuffer)
{
	if (!device || !vkDevice)
		return false;

	DestroySharedBuffer(a_outBuffer);

	// 1. Create buffer in D3D12
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = a_size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = a_d3dFlags;

	D3D12_HEAP_PROPERTIES heapProps{};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	winrt::com_ptr<ID3D12Resource> d3dRes;
	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_SHARED,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(d3dRes.put()));

	if (FAILED(hr) || !d3dRes) {
		logger::error("[D3D12Interop] CreateCommittedResource for buffer failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 2. Export shared NT handle
	HANDLE sharedHandle = nullptr;
	hr = device->CreateSharedHandle(d3dRes.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
	if (FAILED(hr) || !sharedHandle) {
		logger::error("[D3D12Interop] CreateSharedHandle for buffer failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 3. Create Vulkan buffer with external memory info
	VkExternalMemoryBufferCreateInfo extBufInfo{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
	extBufInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

	VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufInfo.pNext = &extBufInfo;
	bufInfo.size = a_size;
	bufInfo.usage = a_vkUsage;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer vkBuf = VK_NULL_HANDLE;
	if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &vkBuf) != VK_SUCCESS) {
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkCreateBuffer with external memory failed");
		return false;
	}

	// 4. Query memory type bits
	VkMemoryWin32HandlePropertiesKHR handleProps{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
	if (vkGetMemoryWin32HandlePropertiesKHR(vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, sharedHandle, &handleProps) != VK_SUCCESS) {
		vkDestroyBuffer(vkDevice, vkBuf, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkGetMemoryWin32HandlePropertiesKHR failed for buffer");
		return false;
	}

	VkMemoryRequirements memReqs{};
	vkGetBufferMemoryRequirements(vkDevice, vkBuf, &memReqs);

	uint32_t memTypeIndex = FindMemoryType(handleProps.memoryTypeBits & memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memTypeIndex == UINT32_MAX) {
		vkDestroyBuffer(vkDevice, vkBuf, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] No compatible memory type for imported D3D12 buffer");
		return false;
	}

	// 5. Import memory and bind to buffer
	VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
	importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
	importInfo.handle = sharedHandle;

	VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.pNext = &importInfo;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = memTypeIndex;

	VkDeviceMemory vkMem = VK_NULL_HANDLE;
	if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &vkMem) != VK_SUCCESS) {
		vkDestroyBuffer(vkDevice, vkBuf, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkAllocateMemory for imported buffer failed");
		return false;
	}

	vkBindBufferMemory(vkDevice, vkBuf, vkMem, 0);

	a_outBuffer.d3dResource = std::move(d3dRes);
	a_outBuffer.sharedHandle = sharedHandle;
	a_outBuffer.vkBuffer = vkBuf;
	a_outBuffer.vkMemory = vkMem;
	a_outBuffer.size = a_size;

	return true;
}

void D3D12Interop::DestroySharedBuffer(SharedBuffer& a_buffer)
{
	if (vkDevice) {
		if (a_buffer.vkBuffer) {
			vkDestroyBuffer(vkDevice, a_buffer.vkBuffer, nullptr);
			a_buffer.vkBuffer = VK_NULL_HANDLE;
		}
		if (a_buffer.vkMemory) {
			vkFreeMemory(vkDevice, a_buffer.vkMemory, nullptr);
			a_buffer.vkMemory = VK_NULL_HANDLE;
		}
	}

	if (a_buffer.sharedHandle) {
		CloseHandle(a_buffer.sharedHandle);
		a_buffer.sharedHandle = nullptr;
	}

	a_buffer.d3dResource = nullptr;
	a_buffer.size = 0;
}

bool D3D12Interop::CreateSharedFence(SharedFence& a_outFence, uint64_t a_initialValue)
{
	if (!device || !vkDevice) {
		logger::error("[D3D12Interop] CreateSharedFence called with null device (d3d={} vk={})", device != nullptr, vkDevice != VK_NULL_HANDLE);
		return false;
	}

	DestroySharedFence(a_outFence);

	// 1. Create shared fence in D3D12
	winrt::com_ptr<ID3D12Fence> d3dFence;
	HRESULT hr = device->CreateFence(a_initialValue, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3dFence.put()));
	if (FAILED(hr) || !d3dFence) {
		logger::error("[D3D12Interop] CreateFence failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 2. Export shared NT handle
	HANDLE sharedHandle = nullptr;
	hr = device->CreateSharedHandle(d3dFence.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
	if (FAILED(hr) || !sharedHandle) {
		logger::error("[D3D12Interop] CreateSharedHandle for fence failed ({:#x})", static_cast<uint32_t>(hr));
		return false;
	}

	// 3. Create Vulkan timeline semaphore and import fence handle
	VkSemaphoreTypeCreateInfo timelineInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	timelineInfo.initialValue = a_initialValue;

	VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	semInfo.pNext = &timelineInfo;

	VkSemaphore vkSem = VK_NULL_HANDLE;
	VkResult vkRes = vkCreateSemaphore(vkDevice, &semInfo, nullptr, &vkSem);
	if (vkRes != VK_SUCCESS) {
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkCreateSemaphore for timeline semaphore failed ({})", static_cast<int>(vkRes));
		return false;
	}

	VkImportSemaphoreWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
	importInfo.semaphore = vkSem;
	importInfo.flags = 0;
	importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
	importInfo.handle = sharedHandle;

	vkRes = vkImportSemaphoreWin32HandleKHR(vkDevice, &importInfo);
	if (vkRes != VK_SUCCESS) {
		vkDestroySemaphore(vkDevice, vkSem, nullptr);
		CloseHandle(sharedHandle);
		logger::error("[D3D12Interop] vkImportSemaphoreWin32HandleKHR failed ({})", static_cast<int>(vkRes));
		return false;
	}

	a_outFence.d3dFence = std::move(d3dFence);
	a_outFence.sharedHandle = sharedHandle;
	a_outFence.vkTimelineSemaphore = vkSem;
	a_outFence.value = a_initialValue;

	return true;
}

void D3D12Interop::DestroySharedFence(SharedFence& a_fence)
{
	if (vkDevice && a_fence.vkTimelineSemaphore) {
		vkDestroySemaphore(vkDevice, a_fence.vkTimelineSemaphore, nullptr);
		a_fence.vkTimelineSemaphore = VK_NULL_HANDLE;
	}

	if (a_fence.sharedHandle) {
		CloseHandle(a_fence.sharedHandle);
		a_fence.sharedHandle = nullptr;
	}

	a_fence.d3dFence = nullptr;
	a_fence.value = 0;
}

ID3D12GraphicsCommandList* D3D12Interop::BeginCommandList(uint32_t a_frameIndex)
{
	if (!available || a_frameIndex >= kMaxFramesInFlight)
		return nullptr;

	auto& allocator = commandAllocators[a_frameIndex];
	auto& cmdList = commandLists[a_frameIndex];

	allocator->Reset();
	cmdList->Reset(allocator.get(), nullptr);
	return cmdList.get();
}

bool D3D12Interop::ExecuteCommandList(uint32_t a_frameIndex)
{
	if (!available || a_frameIndex >= kMaxFramesInFlight)
		return false;

	auto& cmdList = commandLists[a_frameIndex];
	HRESULT hr = cmdList->Close();
	if (FAILED(hr)) {
		logger::error("[D3D12Interop] CommandList[{}] Close failed ({:#x})", a_frameIndex, static_cast<uint32_t>(hr));
		return false;
	}

	ID3D12CommandList* lists[] = { cmdList.get() };
	commandQueue->ExecuteCommandLists(1, lists);
	return true;
}

void D3D12Interop::D3D12QueueWait(const SharedFence& a_fence, uint64_t a_value)
{
	if (commandQueue && a_fence.d3dFence) {
		commandQueue->Wait(a_fence.d3dFence.get(), a_value);
	}
}

void D3D12Interop::D3D12QueueSignal(const SharedFence& a_fence, uint64_t a_value)
{
	if (commandQueue && a_fence.d3dFence) {
		commandQueue->Signal(a_fence.d3dFence.get(), a_value);
	}
}

bool D3D12Interop::VulkanSignalFence(const SharedFence& a_fence, uint64_t a_value)
{
	if (!vkDevice || !a_fence.vkTimelineSemaphore || !vkSignalSemaphoreKHR)
		return false;

	VkSemaphoreSignalInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
	signalInfo.semaphore = a_fence.vkTimelineSemaphore;
	signalInfo.value = a_value;
	return vkSignalSemaphoreKHR(vkDevice, &signalInfo) == VK_SUCCESS;
}

bool D3D12Interop::VulkanWaitFence(const SharedFence& a_fence, uint64_t a_value, uint64_t a_timeoutNs)
{
	if (!vkDevice || !a_fence.vkTimelineSemaphore || !vkWaitSemaphoresKHR)
		return false;

	VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &a_fence.vkTimelineSemaphore;
	waitInfo.pValues = &a_value;
	return vkWaitSemaphoresKHR(vkDevice, &waitInfo, a_timeoutNs) == VK_SUCCESS;
}

uint64_t D3D12Interop::AdvanceTimelineFrame()
{
	std::lock_guard lock(timelineMutex);
	const uint64_t base = timelineCounter;
	timelineCounter += 4;
	return base;
}
