#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <winrt/base.h>

/**
 * @brief Manages D3D12 device matching, cross-API shared NT memory (VK_KHR_external_memory_win32),
 * and shared timeline semaphores (VK_KHR_external_semaphore_win32) for zero-copy GPU interop with DXVK/Vulkan.
 */
class D3D12Interop
{
public:
	static constexpr uint32_t kMaxFramesInFlight = 3;

	struct SharedTexture
	{
		winrt::com_ptr<ID3D12Resource> d3dResource;
		HANDLE sharedHandle = nullptr;
		VkImage vkImage = VK_NULL_HANDLE;
		VkDeviceMemory vkMemory = VK_NULL_HANDLE;
		VkImageView vkView = VK_NULL_HANDLE;
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
		VkFormat vkFormat = VK_FORMAT_UNDEFINED;

		explicit operator bool() const noexcept
		{
			return d3dResource != nullptr && vkImage != VK_NULL_HANDLE;
		}
	};

	struct SharedBuffer
	{
		winrt::com_ptr<ID3D12Resource> d3dResource;
		HANDLE sharedHandle = nullptr;
		VkBuffer vkBuffer = VK_NULL_HANDLE;
		VkDeviceMemory vkMemory = VK_NULL_HANDLE;
		uint64_t size = 0;

		explicit operator bool() const noexcept
		{
			return d3dResource != nullptr && vkBuffer != VK_NULL_HANDLE;
		}
	};

	struct SharedFence
	{
		winrt::com_ptr<ID3D12Fence> d3dFence;
		HANDLE sharedHandle = nullptr;
		VkSemaphore vkTimelineSemaphore = VK_NULL_HANDLE;
		uint64_t value = 0;

		explicit operator bool() const noexcept
		{
			return d3dFence != nullptr && vkTimelineSemaphore != VK_NULL_HANDLE;
		}
	};

	static D3D12Interop* GetSingleton();

	/**
	 * @brief Matches DXVK's Vulkan device LUID, creates the D3D12 device, command queue, and command allocators.
	 */
	bool Initialize(VkInstance a_vkInstance, VkPhysicalDevice a_vkPhysDev, VkDevice a_vkDevice);

	/**
	 * @brief Tears down D3D12 command resources and device references.
	 */
	void Shutdown();

	[[nodiscard]] bool IsAvailable() const noexcept { return available; }

	ID3D12Device5* GetDevice() const noexcept { return device.get(); }
	ID3D12CommandQueue* GetCommandQueue() const noexcept { return commandQueue.get(); }
	VkDevice GetVkDevice() const noexcept { return vkDevice; }

	/**
	 * @brief Creates a 2D texture in D3D12 and imports it into Vulkan via an NT shared handle.
	 */
	bool CreateSharedTexture2D(
		uint32_t a_width, uint32_t a_height,
		DXGI_FORMAT a_dxgiFormat, VkFormat a_vkFormat,
		D3D12_RESOURCE_FLAGS a_d3dFlags, VkImageUsageFlags a_vkUsage,
		SharedTexture& a_outTexture);

	void DestroySharedTexture(SharedTexture& a_texture);

	/**
	 * @brief Creates a buffer in D3D12 and imports it into Vulkan via an NT shared handle.
	 */
	bool CreateSharedBuffer(
		uint64_t a_size,
		D3D12_RESOURCE_FLAGS a_d3dFlags, VkBufferUsageFlags a_vkUsage,
		SharedBuffer& a_outBuffer);

	void DestroySharedBuffer(SharedBuffer& a_buffer);

	/**
	 * @brief Creates a shared fence in D3D12 and imports it into Vulkan as a timeline semaphore.
	 */
	bool CreateSharedFence(SharedFence& a_outFence, uint64_t a_initialValue = 0);

	void DestroySharedFence(SharedFence& a_fence);

	/**
	 * @brief Begins recording commands for a frame slot on the D3D12 direct queue.
	 */
	ID3D12GraphicsCommandList* BeginCommandList(uint32_t a_frameIndex);

	/**
	 * @brief Closes and submits the recorded command list to the D3D12 queue.
	 */
	bool ExecuteCommandList(uint32_t a_frameIndex);

	/**
	 * @brief Queues a GPU wait on the D3D12 command queue for a fence value.
	 */
	void D3D12QueueWait(const SharedFence& a_fence, uint64_t a_value);

	/**
	 * @brief Queues a GPU signal on the D3D12 command queue for a fence value.
	 */
	void D3D12QueueSignal(const SharedFence& a_fence, uint64_t a_value);

	/**
	 * @brief Returns the shared timeline fence owned by the interop singleton.
	 */
	SharedFence& GetMainSharedFence() noexcept { return mainFence; }

	/**
	 * @brief Signals a shared timeline fence from the Vulkan API.
	 */
	bool VulkanSignalFence(const SharedFence& a_fence, uint64_t a_value);

	/**
	 * @brief Waits on a shared timeline fence from the Vulkan API.
	 */
	bool VulkanWaitFence(const SharedFence& a_fence, uint64_t a_value, uint64_t a_timeoutNs = UINT64_MAX);

	/**
	 * @brief Advances the timeline sequence and returns the next monotonic fence values.
	 * Returns the base fence value for the frame:
	 *   base + 1: Vulkan signals after RT -> D3D12 waits before FSR-RR
	 *   base + 2: D3D12 signals after FSR-RR -> Vulkan waits before composite
	 *   base + 3: Vulkan signals after post -> D3D12 waits before FSR 4
	 *   base + 4: D3D12 signals after FSR 4 -> Vulkan waits before present
	 */
	uint64_t AdvanceTimelineFrame();

private:
	D3D12Interop() = default;
	~D3D12Interop();

	D3D12Interop(const D3D12Interop&) = delete;
	D3D12Interop& operator=(const D3D12Interop&) = delete;

	uint32_t FindMemoryType(uint32_t a_typeBits, VkMemoryPropertyFlags a_properties) const;

	bool available = false;

	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;

	PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
	PFN_vkSignalSemaphoreKHR vkSignalSemaphoreKHR = nullptr;
	PFN_vkWaitSemaphoresKHR vkWaitSemaphoresKHR = nullptr;

	winrt::com_ptr<IDXGIFactory4> dxgiFactory;
	winrt::com_ptr<IDXGIAdapter1> dxgiAdapter;
	winrt::com_ptr<ID3D12Device5> device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[kMaxFramesInFlight];
	winrt::com_ptr<ID3D12GraphicsCommandList> commandLists[kMaxFramesInFlight];

	SharedFence mainFence;
	uint64_t timelineCounter = 0;
	std::mutex timelineMutex;
};
