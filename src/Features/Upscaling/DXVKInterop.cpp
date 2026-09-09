#include "DXVKInterop.h"

#include "Globals.h"

#include <algorithm>

namespace
{
	struct QueueSubmitAttempt
	{
		VkResult endResult = VK_ERROR_DEVICE_LOST;
		VkResult resetResult = VK_ERROR_DEVICE_LOST;
		VkResult submitResult = VK_ERROR_DEVICE_LOST;
		DWORD exceptionCode = 0;
		bool queueLockAcquired = false;
		bool faulted = false;
	};

	struct VulkanResultAttempt
	{
		VkResult result = VK_ERROR_DEVICE_LOST;
	};

	VulkanResultAttempt CreateCommandPoolSEH(VkDevice a_device,
		const VkCommandPoolCreateInfo* a_createInfo, VkCommandPool* a_commandPool) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkCreateCommandPool(a_device, a_createInfo, nullptr, a_commandPool);
		return attempt;
	}

	VulkanResultAttempt AllocateCommandBuffersSEH(VkDevice a_device,
		const VkCommandBufferAllocateInfo* a_allocateInfo, VkCommandBuffer* a_commandBuffers) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkAllocateCommandBuffers(a_device, a_allocateInfo, a_commandBuffers);
		return attempt;
	}

	VulkanResultAttempt GetFenceStatusSEH(VkDevice a_device, VkFence a_fence) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkGetFenceStatus(a_device, a_fence);
		return attempt;
	}

	VulkanResultAttempt WaitForFenceSEH(VkDevice a_device, VkFence a_fence) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkWaitForFences(a_device, 1, &a_fence, VK_TRUE, UINT64_MAX);
		return attempt;
	}

	VulkanResultAttempt ResetCommandBufferSEH(VkCommandBuffer a_commandBuffer) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkResetCommandBuffer(a_commandBuffer, 0);
		return attempt;
	}

	VulkanResultAttempt BeginCommandBufferSEH(VkCommandBuffer a_commandBuffer,
		const VkCommandBufferBeginInfo* a_beginInfo) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkBeginCommandBuffer(a_commandBuffer, a_beginInfo);
		return attempt;
	}

	void DestroyImageView(PFN_vkDestroyImageView a_destroyImageView,
		VkDevice a_device, VkImageView a_view) noexcept
	{
		if (a_destroyImageView && a_view != VK_NULL_HANDLE)
			a_destroyImageView(a_device, a_view, nullptr);
	}

	void DestroyCommandPool(VkDevice a_device, VkCommandPool a_commandPool) noexcept
	{
		if (a_commandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(a_device, a_commandPool, nullptr);
	}

	void FreeCommandBuffers(VkDevice a_device, VkCommandPool a_commandPool,
		uint32_t a_count, const VkCommandBuffer* a_commandBuffers) noexcept
	{
		if (a_count)
			vkFreeCommandBuffers(a_device, a_commandPool, a_count, a_commandBuffers);
	}

	void ReleaseSubmissionQueue(IDXGIVkInteropDevice* a_interopDevice) noexcept
	{
		a_interopDevice->ReleaseSubmissionQueue();
	}

	/// Submits a recorded command buffer directly on DXVK's queue through the COM
	/// interop device. Flushes D3D11 rendering first so our command buffer is ordered
	/// after it, then takes the submission-queue lock for the submit.
	///
	/// FlushRenderingCommands orders this behind the D3D11 work already recorded, which is
	/// what frame generation needs: it reads the results of this buffer. Validated in game
	/// with both FSR-FG and DLSS-G presenting at an exact 2x.
	QueueSubmitAttempt DirectQueueSubmitSEH(IDXGIVkInteropDevice* a_interopDevice,
		VkDevice a_device, VkQueue a_queue,
		VkCommandBuffer a_commandBuffer, VkFence a_fence) noexcept
	{
		QueueSubmitAttempt attempt{};
		__try {
			attempt.endResult = vkEndCommandBuffer(a_commandBuffer);
			if (attempt.endResult == VK_SUCCESS) {
				attempt.resetResult = vkResetFences(a_device, 1, &a_fence);
				if (attempt.resetResult == VK_SUCCESS) {
					a_interopDevice->FlushRenderingCommands();
					__try {
						a_interopDevice->LockSubmissionQueue();
						attempt.queueLockAcquired = true;
						VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
						submitInfo.commandBufferCount = 1;
						submitInfo.pCommandBuffers = &a_commandBuffer;
						attempt.submitResult = vkQueueSubmit(a_queue, 1, &submitInfo, a_fence);
					} __finally {
						if (attempt.queueLockAcquired)
							ReleaseSubmissionQueue(a_interopDevice);
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attempt.faulted = true;
			attempt.exceptionCode = GetExceptionCode();
		}
		return attempt;
	}

	struct DeviceIdleAttempt
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;
		DWORD exceptionCode = 0;
		bool queueLockAcquired = false;
		bool functionAvailable = false;
		bool faulted = false;
	};

	DeviceIdleAttempt WaitDeviceIdleSEH(IDXGIVkInteropDevice* a_interopDevice,
		PFN_vkGetDeviceProcAddr a_getDeviceProcAddr, VkDevice a_device) noexcept
	{
		DeviceIdleAttempt attempt{};
		__try {
			a_interopDevice->FlushRenderingCommands();
			__try {
				a_interopDevice->LockSubmissionQueue();
				attempt.queueLockAcquired = true;
				auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
					a_getDeviceProcAddr(a_device, "vkDeviceWaitIdle"));
				attempt.functionAvailable = waitIdle != nullptr;
				if (waitIdle)
					attempt.result = waitIdle(a_device);
			} __finally {
				if (attempt.queueLockAcquired)
					ReleaseSubmissionQueue(a_interopDevice);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attempt.faulted = true;
			attempt.exceptionCode = GetExceptionCode();
		}
		return attempt;
	}

	VkResult CreateSignaledFence(VkDevice a_device, VkFence* a_fence) noexcept
	{
		*a_fence = VK_NULL_HANDLE;
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		return vkCreateFence(a_device, &fenceInfo, nullptr, a_fence);
	}

	void DestroyFence(VkDevice a_device, VkFence a_fence) noexcept
	{
		if (a_fence != VK_NULL_HANDLE)
			vkDestroyFence(a_device, a_fence, nullptr);
	}
}

DXVKInterop* DXVKInterop::GetSingleton()
{
	static DXVKInterop singleton;
	return &singleton;
}

VkColorSpaceKHR DXVKInterop::RequestedPresenterColorSpace(bool a_hdr)
{
	return a_hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

DXVKInterop::PresenterEncoding DXVKInterop::ClassifyPresenterEncoding(const PresenterSurfaceState& a_state)
{
	if (!a_state.serial)
		return PresenterEncoding::kUnknown;

	if (a_state.requestedColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
		a_state.effectiveColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		return PresenterEncoding::kSDR;

	if (a_state.requestedColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
		if (a_state.effectiveColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
			return PresenterEncoding::kHDR10;
		if (a_state.effectiveColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
			return PresenterEncoding::kHDR10ScRGBFallback;
	}

	return PresenterEncoding::kUnknown;
}

bool DXVKInterop::PresenterStateMatches(
	const PresenterSurfaceState& a_state, VkColorSpaceKHR a_requestedColorSpace)
{
	if (!a_state.serial || a_state.requestedColorSpace != a_requestedColorSpace)
		return false;

	const PresenterEncoding encoding = ClassifyPresenterEncoding(a_state);
	if (a_requestedColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
		return encoding == PresenterEncoding::kHDR10 ||
		       encoding == PresenterEncoding::kHDR10ScRGBFallback;
	return a_requestedColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
	       encoding == PresenterEncoding::kSDR;
}

bool DXVKInterop::RefreshPresenterSurfaceState()
{
	if (!getPresenterSurfaceState)
		return false;

	uint32_t format = VK_FORMAT_UNDEFINED;
	uint32_t requestedColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	uint32_t effectiveColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	const uint64_t serial = getPresenterSurfaceState(&format, &requestedColorSpace, &effectiveColorSpace);
	if (!serial)
		return false;

	std::lock_guard lock(presenterStateMutex);
	if (serial <= observedPresenterState.serial)
		return false;

	observedPresenterState.serial = serial;
	observedPresenterState.format = static_cast<VkFormat>(format);
	observedPresenterState.requestedColorSpace = static_cast<VkColorSpaceKHR>(requestedColorSpace);
	observedPresenterState.effectiveColorSpace = static_cast<VkColorSpaceKHR>(effectiveColorSpace);
	logger::info("[DXVKInterop] Observed presenter surface serial {}: format={}, requestedColorSpace={}, effectiveColorSpace={}",
		serial, format, requestedColorSpace, effectiveColorSpace);
	return true;
}

void DXVKInterop::CommitPresenterSurfaceStateForRenderFrame()
{
	std::lock_guard lock(presenterStateMutex);

	if (presenterTransitionPending) {
		const bool satisfied =
			observedPresenterState.serial > presenterTransitionBaselineSerial &&
			observedPresenterState.requestedColorSpace == presenterTransitionRequestedColorSpace;
		if (!satisfied) {
			if (++presenterTransitionFrameCount > 120) {
				logger::warn("[DXVKInterop] presenter color-space transition timed out after {} frames; cancelling",
					presenterTransitionFrameCount);
				presenterTransitionPending = false;
				presenterTransitionFrameCount = 0;
			}
			return;
		}
	}

	if (!observedPresenterState.serial ||
		observedPresenterState.serial <= committedPresenterState.serial)
		return;

	if (presenterTransitionPending) {
		presenterTransitionPending = false;
		presenterTransitionFrameCount = 0;
	}

	committedPresenterState = observedPresenterState;
	logger::info("[DXVKInterop] Committed presenter surface serial {} for render frames",
		committedPresenterState.serial);
	if (ClassifyPresenterEncoding(committedPresenterState) == PresenterEncoding::kHDR10ScRGBFallback) {
		logger::warn("[DXVKInterop] HDR frame generation disabled for the scRGB presenter fallback; "
		             "a HUD-less image rendered directly in the presenter encoding is required");
	}
}

void DXVKInterop::BeginPresenterColorSpaceTransition(bool a_hdr, bool a_requireNewSerial)
{
	const VkColorSpaceKHR requestedColorSpace = RequestedPresenterColorSpace(a_hdr);
	std::lock_guard lock(presenterStateMutex);

	if (presenterTransitionPending &&
		presenterTransitionRequestedColorSpace == requestedColorSpace)
		return;

	if (!presenterTransitionPending && !a_requireNewSerial &&
		(PresenterStateMatches(committedPresenterState, requestedColorSpace) ||
		 PresenterStateMatches(observedPresenterState, requestedColorSpace)))
		return;

	presenterTransitionPending = true;
	presenterTransitionFrameCount = 0;
	presenterTransitionRequestedColorSpace = requestedColorSpace;
	presenterTransitionBaselineSerial = observedPresenterState.serial > committedPresenterState.serial
	                                    ? observedPresenterState.serial
	                                    : committedPresenterState.serial;
	logger::info("[DXVKInterop] Presenter color-space transition started: target={}, baselineSerial={}",
		static_cast<uint32_t>(requestedColorSpace), presenterTransitionBaselineSerial);
}

void DXVKInterop::CancelPresenterColorSpaceTransition(bool a_hdr)
{
	const VkColorSpaceKHR requestedColorSpace = RequestedPresenterColorSpace(a_hdr);
	std::lock_guard lock(presenterStateMutex);
	if (presenterTransitionPending &&
		presenterTransitionRequestedColorSpace == requestedColorSpace) {
		presenterTransitionPending = false;
		logger::warn("[DXVKInterop] Presenter color-space transition cancelled for target={}",
			static_cast<uint32_t>(requestedColorSpace));
	}
}

DXVKInterop::PresenterEncoding DXVKInterop::GetPresenterEncodingForFrame() const
{
	std::lock_guard lock(presenterStateMutex);
	return presenterTransitionPending ? PresenterEncoding::kUnknown :
	                                    ClassifyPresenterEncoding(committedPresenterState);
}

VkFormat DXVKInterop::GetPresenterFormatForFrame() const
{
	std::lock_guard lock(presenterStateMutex);
	return presenterTransitionPending ? VK_FORMAT_UNDEFINED : committedPresenterState.format;
}

bool DXVKInterop::IsPresenterStateReadyForFrame(bool a_hdr) const
{
	std::lock_guard lock(presenterStateMutex);
	if (presenterTransitionPending ||
		!PresenterStateMatches(committedPresenterState, RequestedPresenterColorSpace(a_hdr)))
		return false;
	return ClassifyPresenterEncoding(committedPresenterState) != PresenterEncoding::kHDR10ScRGBFallback;
}

bool DXVKInterop::Initialize()
{
	if (available)
		return true;

	auto d3dDevice = globals::d3d::device;
	if (!d3dDevice) {
		logger::warn("[DXVKInterop] No D3D11 device available yet");
		return false;
	}

	winrt::com_ptr<IDXGIVkInteropDevice> dev;
	if (FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), dev.put_void()))) {
		logger::info("[DXVKInterop] IDXGIVkInteropDevice not present — not running under DXVK");
		return false;
	}

	interopDevice = dev;
	interopDevice->GetVulkanHandles(&instance, &physicalDevice, &device);
	interopDevice->GetSubmissionQueue(&queue, &queueFamilyIndex);

	if (!instance || !physicalDevice || !device || !queue) {
		logger::error("[DXVKInterop] DXVK returned null Vulkan handles");
		interopDevice = nullptr;
		return false;
	}

	if (HMODULE vk = GetModuleHandleW(L"vulkan-1.dll")) {
		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			reinterpret_cast<void*>(GetProcAddress(vk, "vkGetInstanceProcAddr")));
	}
	if (!vkGetInstanceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetInstanceProcAddr from vulkan-1.dll");
		interopDevice = nullptr;
		return false;
	}

	vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
		vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
	if (!vkGetDeviceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetDeviceProcAddr");
		interopDevice = nullptr;
		return false;
	}

	vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
		vkGetDeviceProcAddr(device, "vkDestroyImageView"));

	if (HMODULE module = GetModuleHandleW(L"dxvk_d3d11.dll")) {
		synchronousPresentControlAvailable = GetProcAddress(module, "dxvkSetSyncPresent") != nullptr;
		getPresenterSurfaceState = reinterpret_cast<GetPresenterSurfaceStateFn>(
			GetProcAddress(module, "dxvkGetPresenterSurfaceState"));
	}
	if (!synchronousPresentControlAvailable)
		logger::warn("[DXVKInterop] dxvkSetSyncPresent is unavailable - DLSS-G disabled");
	if (!getPresenterSurfaceState)
		logger::warn("[DXVKInterop] dxvkGetPresenterSurfaceState is unavailable - frame generation disabled");

	if (auto pfnProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"))) {
		VkPhysicalDeviceProperties props{};
		pfnProps(physicalDevice, &props);
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device: '{}' (API {}.{}.{}), queueFamily {}",
			props.deviceName,
			VK_API_VERSION_MAJOR(props.apiVersion),
			VK_API_VERSION_MINOR(props.apiVersion),
			VK_API_VERSION_PATCH(props.apiVersion),
			queueFamilyIndex);
	} else {
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device (queueFamily {})", queueFamilyIndex);
	}

	available = true;
	return true;
}

bool DXVKInterop::GetSubmissionQueue1(VkQueue* a_outQueue, uint32_t* a_outQueueIndex, uint32_t* a_outQueueFamilyIndex) const
{
	if (!interopDevice)
		return false;

	winrt::com_ptr<IDXGIVkInteropDevice1> interopDevice1;
	if (SUCCEEDED(interopDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice1), interopDevice1.put_void()))) {
		interopDevice1->GetSubmissionQueue1(a_outQueue, a_outQueueIndex, a_outQueueFamilyIndex);
		return true;
	}

	if (a_outQueue)
		*a_outQueue = queue;
	if (a_outQueueIndex)
		*a_outQueueIndex = 0;
	if (a_outQueueFamilyIndex)
		*a_outQueueFamilyIndex = queueFamilyIndex;
	return true;
}

bool DXVKInterop::GetVkImage(ID3D11Resource* a_resource, VkImage* a_outImage,
	VkImageLayout* a_outLayout, VkImageCreateInfo* a_outInfo) const
{
	if (!available || !a_resource)
		return false;

	winrt::com_ptr<IDXGIVkInteropSurface> surface;
	if (FAILED(a_resource->QueryInterface(__uuidof(IDXGIVkInteropSurface), surface.put_void())))
		return false;

	VkImageCreateInfo localInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageCreateInfo* info = a_outInfo ? a_outInfo : &localInfo;
	info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info->pNext = nullptr;
	info->queueFamilyIndexCount = 0;
	info->pQueueFamilyIndices = nullptr;

	return SUCCEEDED(surface->GetVulkanImageInfo(a_outImage, a_outLayout, info));
}

bool DXVKInterop::GetVkBuffer(ID3D11Resource* a_resource, VkBuffer* a_outBuffer,
	VkDeviceSize* a_outOffset, VkDeviceSize* a_outLength, VkDeviceAddress* a_outGpuAddress) const
{
	if (!available || !a_resource)
		return false;

	winrt::com_ptr<IDXGIVkInteropBuffer> buffer;
	if (FAILED(a_resource->QueryInterface(__uuidof(IDXGIVkInteropBuffer), buffer.put_void())))
		return false;

	return SUCCEEDED(buffer->GetVulkanBufferInfo(a_outBuffer, a_outOffset, a_outLength, a_outGpuAddress));
}

bool DXVKInterop::WaitDeviceIdle()
{
	std::lock_guard lock(commandRingMutex);
	if (!interopDevice || !vkGetDeviceProcAddr || device == VK_NULL_HANDLE)
		return false;
	if (deviceLost)
		return true;

	const DeviceIdleAttempt attempt = WaitDeviceIdleSEH(interopDevice.get(), vkGetDeviceProcAddr, device);
	if (attempt.faulted) {
		logger::error("[DXVKInterop] device-idle synchronization faulted (SEH {:#x})", attempt.exceptionCode);
		return false;
	} else if (!attempt.functionAvailable) {
		logger::error("[DXVKInterop] vkDeviceWaitIdle is unavailable");
		return false;
	} else if (attempt.result != VK_SUCCESS) {
		if (attempt.result == VK_ERROR_DEVICE_LOST) {
			deviceLost = true;
			commandRingFaulted = true;
			logger::critical("[DXVKInterop] device lost; Vulkan interop is unavailable for this session");
			return true;
		}
		logger::error("[DXVKInterop] vkDeviceWaitIdle failed ({})", static_cast<int>(attempt.result));
		return false;
	}
	return true;
}

bool DXVKInterop::CreateCommandResources(uint32_t a_framesInFlight)
{
	std::lock_guard lock(commandRingMutex);
	if (!available)
		return false;
	if (commandPool != VK_NULL_HANDLE)
		return !commandRingFaulted;

	commandRingFaulted = false;

	framesInFlight = a_framesInFlight ? a_framesInFlight : 1;

	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	VkCommandPool createdCommandPool = VK_NULL_HANDLE;
	const VulkanResultAttempt poolAttempt = CreateCommandPoolSEH(device, &poolInfo, &createdCommandPool);
	const bool poolCreated = poolAttempt.result == VK_SUCCESS;
	if (!poolCreated) {
		createdCommandPool = VK_NULL_HANDLE;
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkCreateCommandPool failed ({})",
			static_cast<int>(poolAttempt.result));
		return false;
	}
	commandPool = createdCommandPool;

	std::vector<VkCommandBuffer> allocatedCommandBuffers(framesInFlight, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = framesInFlight;
	const VulkanResultAttempt allocateAttempt =
		AllocateCommandBuffersSEH(device, &allocInfo, allocatedCommandBuffers.data());
	const bool commandBuffersAllocated =
		allocateAttempt.result == VK_SUCCESS;
	if (!commandBuffersAllocated) {
		std::fill(allocatedCommandBuffers.begin(), allocatedCommandBuffers.end(), VK_NULL_HANDLE);
		logger::error("[DXVKInterop] vkAllocateCommandBuffers failed ({})",
			static_cast<int>(allocateAttempt.result));
		commandRingFaulted = true;
		DestroyCommandResources();
		commandRingFaulted = true;
		return false;
	}
	commandBuffers = std::move(allocatedCommandBuffers);

	commandFences.resize(framesInFlight, VK_NULL_HANDLE);
	for (uint32_t i = 0; i < framesInFlight; ++i) {
		VkFence createdFence = VK_NULL_HANDLE;
		const VkResult result = CreateSignaledFence(device, &createdFence);
		if (result != VK_SUCCESS) {
			createdFence = VK_NULL_HANDLE;
			logger::error("[DXVKInterop] vkCreateFence failed ({})", static_cast<int>(result));
			commandRingFaulted = true;
			DestroyCommandResources();
			return false;
		}
		commandFences[i] = createdFence;
	}

	commandFrameIndex = 0;
	pendingViewDeletes.assign(framesInFlight, {});
	pendingResourceReleases.assign(framesInFlight, {});
	logger::info("[DXVKInterop] Command ring created ({} frames in flight, queueFamily {})", framesInFlight, queueFamilyIndex);
	return true;
}

void DXVKInterop::DestroyCommandResources()
{
	std::lock_guard lock(commandRingMutex);
	if (device == VK_NULL_HANDLE)
		return;
	if (commandRingFaulted && !WaitDeviceIdle()) {
		logger::error("[DXVKInterop] command resources remain quarantined because device idle could not be proven");
		return;
	}

	if (!commandRingFaulted) {
		for (auto f : commandFences) {
			if (f == VK_NULL_HANDLE)
				continue;
			const VulkanResultAttempt waitAttempt = WaitForFenceSEH(device, f);
			if (waitAttempt.result != VK_SUCCESS) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] command-resource fence wait failed ({})",
					static_cast<int>(waitAttempt.result));
				return;
			}
		}
	}
	if (!vkDestroyImageView) {
		for (const auto& slot : pendingViewDeletes) {
			if (std::find_if(slot.begin(), slot.end(),
					[](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != slot.end()) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkDestroyImageView is unavailable; command resources remain quarantined");
				return;
			}
		}
	} else {
		for (size_t slotIndex = 0; slotIndex < pendingViewDeletes.size(); ++slotIndex) {
			auto& slot = pendingViewDeletes[slotIndex];
			for (VkImageView& v : slot) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
			slot.clear();
			if (slotIndex < pendingResourceReleases.size())
				pendingResourceReleases[slotIndex].clear();
		}
	}
	pendingViewDeletes.clear();
	pendingResourceReleases.clear();
	for (VkFence& f : commandFences) {
		if (f == VK_NULL_HANDLE)
			continue;
		DestroyFence(device, f);
		f = VK_NULL_HANDLE;
	}
	commandFences.clear();

	if (commandPool != VK_NULL_HANDLE) {
		DestroyCommandPool(device, commandPool);
		commandPool = VK_NULL_HANDLE;
	}
	commandBuffers.clear();
	framesInFlight = 0;
	commandFrameIndex = 0;
	commandRingFaulted = false;
}

bool DXVKInterop::DrainCommandRing()
{
	std::lock_guard lock(commandRingMutex);
	if (commandPool == VK_NULL_HANDLE)
		return true;
	if (device == VK_NULL_HANDLE)
		return false;
	if (commandRingFaulted && !WaitDeviceIdle()) {
		logger::error("[DXVKInterop] command-ring resources remain quarantined because device idle could not be proven");
		return false;
	}

	if (!commandRingFaulted) {
		for (VkFence fence : commandFences) {
			if (fence == VK_NULL_HANDLE)
				continue;
			const VulkanResultAttempt waitAttempt = WaitForFenceSEH(device, fence);
			if (waitAttempt.result != VK_SUCCESS) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] failed to drain a command-ring fence ({})",
					static_cast<int>(waitAttempt.result));
				return false;
			}
		}
	}

	if (!vkDestroyImageView) {
		for (const auto& slot : pendingViewDeletes) {
			if (std::find_if(slot.begin(), slot.end(),
					[](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != slot.end()) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkDestroyImageView is unavailable; deferred views remain quarantined");
				return false;
			}
		}
	} else {
		for (size_t slotIndex = 0; slotIndex < pendingViewDeletes.size(); ++slotIndex) {
			auto& slot = pendingViewDeletes[slotIndex];
			for (VkImageView& v : slot) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
			slot.clear();
			if (slotIndex < pendingResourceReleases.size())
				pendingResourceReleases[slotIndex].clear();
		}
	}
	return true;
}

bool DXVKInterop::CommandResourcesReady() const
{
	std::lock_guard lock(commandRingMutex);
	return commandPool != VK_NULL_HANDLE && !commandRingFaulted;
}

bool DXVKInterop::HasCommandRingFault() const
{
	std::lock_guard lock(commandRingMutex);
	return commandRingFaulted;
}

bool DXVKInterop::RecoverCommandRing()
{
	std::lock_guard lock(commandRingMutex);
	if (!commandRingFaulted)
		return commandPool != VK_NULL_HANDLE;

	const uint32_t ringDepth = framesInFlight ? framesInFlight : 3;
	if (!WaitDeviceIdle())
		return false;
	DestroyCommandResources();
	if (commandPool != VK_NULL_HANDLE)
		return false;
	if (!CreateCommandResources(ringDepth)) {
		logger::error("[DXVKInterop] failed to recreate the quarantined command ring");
		return false;
	}

	logger::warn("[DXVKInterop] recovered the command ring after an ambiguous submission fault");
	return true;
}

bool DXVKInterop::FrameGenerationQueueInteropReady() const
{
	std::lock_guard lock(commandRingMutex);
	return available;
}

DXVKInterop::CommandTransaction DXVKInterop::BeginFrameCommandBuffer()
{
	std::unique_lock ringLock(commandRingMutex);
	if (commandPool == VK_NULL_HANDLE || commandRingFaulted)
		return {};

	constexpr uint32_t kMaxRingDepth = 64;
	uint32_t next = (commandFrameIndex + 1) % framesInFlight;
	VulkanResultAttempt nextFenceAttempt = GetFenceStatusSEH(device, commandFences[next]);
	if ((nextFenceAttempt.result != VK_SUCCESS && nextFenceAttempt.result != VK_NOT_READY)) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkGetFenceStatus failed ({})",
			static_cast<int>(nextFenceAttempt.result));
		return {};
	}
	if (nextFenceAttempt.result == VK_NOT_READY) {
		uint32_t freeSlot = UINT32_MAX;
		for (uint32_t i = 0; i < framesInFlight; ++i) {
			const uint32_t cand = (next + i) % framesInFlight;
			const VulkanResultAttempt candidateAttempt = GetFenceStatusSEH(device, commandFences[cand]);
			if ((candidateAttempt.result != VK_SUCCESS && candidateAttempt.result != VK_NOT_READY)) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkGetFenceStatus failed ({})",
					static_cast<int>(candidateAttempt.result));
				return {};
			}
			if (candidateAttempt.result == VK_SUCCESS) {
				freeSlot = cand;
				break;
			}
		}
		if (freeSlot != UINT32_MAX) {
			next = freeSlot;
		} else if (framesInFlight < kMaxRingDepth) {
			VkCommandBuffer newCb = VK_NULL_HANDLE;
			VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocInfo.commandPool = commandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			VkFence newFence = VK_NULL_HANDLE;
			const VulkanResultAttempt allocateAttempt = AllocateCommandBuffersSEH(device, &allocInfo, &newCb);
			const bool commandBufferAllocated =
				allocateAttempt.result == VK_SUCCESS;
			if (!commandBufferAllocated)
				newCb = VK_NULL_HANDLE;
			const VkResult fenceResult = commandBufferAllocated ?
				CreateSignaledFence(device, &newFence) : allocateAttempt.result;
			const bool fenceCreated = commandBufferAllocated && fenceResult == VK_SUCCESS;
			if (!fenceCreated)
				newFence = VK_NULL_HANDLE;
			if (commandBufferAllocated && fenceCreated) {
				commandBuffers.push_back(newCb);
				commandFences.push_back(newFence);
				pendingViewDeletes.emplace_back();
				pendingResourceReleases.emplace_back();
				next = framesInFlight;
				++framesInFlight;
				logger::info("[DXVKInterop] Command ring grown to {} (all slots in flight)", framesInFlight);
			} else {
				commandRingFaulted = true;
				if (fenceCreated)
					DestroyFence(device, newFence);
				if (commandBufferAllocated)
					FreeCommandBuffers(device, commandPool, 1u, &newCb);
				newFence = VK_NULL_HANDLE;
				newCb = VK_NULL_HANDLE;
				logger::error("[DXVKInterop] command ring growth failed (allocate={}, fence={})",
					static_cast<int>(allocateAttempt.result), static_cast<int>(fenceResult));
				return {};
			}
		} else {
			static bool s_warned = false;
			if (!s_warned) {
				s_warned = true;
				logger::error("[DXVKInterop] command ring reached max depth {} - refusing to block the render thread", kMaxRingDepth);
			}
			return {};
		}
	}
	commandFrameIndex = next;
	VkCommandBuffer cb = commandBuffers[commandFrameIndex];

	if (commandFrameIndex < pendingViewDeletes.size()) {
		auto& dead = pendingViewDeletes[commandFrameIndex];
		if (vkDestroyImageView) {
			for (VkImageView& v : dead) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
		} else if (std::find_if(dead.begin(), dead.end(),
				       [](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != dead.end()) {
			commandRingFaulted = true;
			logger::error("[DXVKInterop] vkDestroyImageView is unavailable; deferred views remain quarantined");
			return {};
		}
		dead.clear();
	}
	if (commandFrameIndex < pendingResourceReleases.size())
		pendingResourceReleases[commandFrameIndex].clear();

	const VulkanResultAttempt resetAttempt = ResetCommandBufferSEH(cb);
	if (resetAttempt.result != VK_SUCCESS) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkResetCommandBuffer failed ({})",
			static_cast<int>(resetAttempt.result));
		return {};
	}

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	const VulkanResultAttempt beginAttempt = BeginCommandBufferSEH(cb, &beginInfo);
	if (beginAttempt.result != VK_SUCCESS) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkBeginCommandBuffer failed ({})",
			static_cast<int>(beginAttempt.result));
		return {};
	}
	return CommandTransaction(this, commandFrameIndex, cb, std::move(ringLock));
}

bool DXVKInterop::SubmitFrameCommandBuffer(CommandTransaction& a_transaction)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		a_transaction.submitted || a_transaction.slot >= commandBuffers.size() ||
		a_transaction.commandBuffer == VK_NULL_HANDLE ||
		commandBuffers[a_transaction.slot] != a_transaction.commandBuffer ||
		commandPool == VK_NULL_HANDLE || commandRingFaulted)
		return false;
	const uint32_t slot = a_transaction.slot;
	const VkCommandBuffer commandBuffer = a_transaction.commandBuffer;

	VkFence& fence = commandFences[slot];

	const QueueSubmitAttempt attempt = DirectQueueSubmitSEH(
		interopDevice.get(), device, queue, commandBuffer, fence);
	if (attempt.faulted || attempt.endResult != VK_SUCCESS ||
		attempt.resetResult != VK_SUCCESS || attempt.submitResult != VK_SUCCESS) {
		commandRingFaulted = true;
		if (attempt.faulted) {
			logger::error("[DXVKInterop] vkQueueSubmit faulted (SEH {:#x})", attempt.exceptionCode);
			a_transaction.submissionMayBeInFlight = attempt.queueLockAcquired;
			return false;
		} else if (attempt.endResult != VK_SUCCESS) {
			logger::error("[DXVKInterop] vkEndCommandBuffer failed ({})",
				static_cast<int>(attempt.endResult));
		} else if (attempt.resetResult != VK_SUCCESS) {
			logger::error("[DXVKInterop] vkResetFences failed ({})",
				static_cast<int>(attempt.resetResult));
		} else {
			logger::error("[DXVKInterop] vkQueueSubmit failed ({})",
				static_cast<int>(attempt.submitResult));
		}
		return false;
	}
	a_transaction.submitted = true;
	return true;
}

void DXVKInterop::QueueViewsForDeferredDelete(const CommandTransaction& a_transaction,
	const VkImageView* a_views, uint32_t a_count)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		(!a_transaction.submitted && !a_transaction.submissionMayBeInFlight) ||
		!a_views || a_transaction.slot >= pendingViewDeletes.size())
		return;
	auto& slot = pendingViewDeletes[a_transaction.slot];
	for (uint32_t i = 0; i < a_count; ++i)
		if (a_views[i] != VK_NULL_HANDLE)
			slot.push_back(a_views[i]);
}

void DXVKInterop::QueueResourcesForDeferredRelease(const CommandTransaction& a_transaction,
	ID3D11Resource* const* a_resources, uint32_t a_count)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		(!a_transaction.submitted && !a_transaction.submissionMayBeInFlight) ||
		!a_resources || a_transaction.slot >= pendingResourceReleases.size())
		return;
	auto& slot = pendingResourceReleases[a_transaction.slot];
	for (uint32_t i = 0; i < a_count; ++i) {
		if (a_resources[i] == nullptr)
			continue;
		winrt::com_ptr<ID3D11Resource> resource;
		resource.copy_from(a_resources[i]);
		slot.push_back(std::move(resource));
	}
}
