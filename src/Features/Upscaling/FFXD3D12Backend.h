#pragma once

#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <winrt/base.h>

#include "D3D12Interop.h"

// FidelityFX SDK host API
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_api_types.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

/**
 * @brief Manages the AMD FidelityFX SDK Direct3D 12 backend interface and shared resource conversion.
 */
class FFXD3D12Backend
{
public:
	static FFXD3D12Backend* GetSingleton();

	bool Initialize(ID3D12Device* a_device);
	void Shutdown();

	[[nodiscard]] bool IsAvailable() const noexcept { return initialized && ffxModule != nullptr; }

	ID3D12Device* GetDevice() const noexcept { return device; }
	const ffxFunctions& GetApi() const noexcept { return ffxApi; }

	ffxCreateBackendDX12Desc GetBackendDesc() const noexcept
	{
		ffxCreateBackendDX12Desc desc{};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		desc.device = device;
		return desc;
	}

	/**
	 * @brief Converts a shared D3D12 texture into an FfxApiResource description.
	 */
	static FfxApiResource CreateResource(
		const D3D12Interop::SharedTexture& a_texture,
		uint32_t a_state = FFX_API_RESOURCE_STATE_COMPUTE_READ,
		uint32_t a_usage = FFX_API_RESOURCE_USAGE_READ_ONLY);

	/**
	 * @brief Converts an ID3D12Resource into an FfxApiResource description.
	 */
	static FfxApiResource CreateResource(
		ID3D12Resource* a_resource,
		uint32_t a_width, uint32_t a_height,
		DXGI_FORMAT a_format,
		uint32_t a_state = FFX_API_RESOURCE_STATE_COMPUTE_READ,
		uint32_t a_usage = FFX_API_RESOURCE_USAGE_READ_ONLY);

private:
	FFXD3D12Backend() = default;
	~FFXD3D12Backend();

	FFXD3D12Backend(const FFXD3D12Backend&) = delete;
	FFXD3D12Backend& operator=(const FFXD3D12Backend&) = delete;

	bool initialized = false;
	HMODULE ffxModule = nullptr;
	ID3D12Device* device = nullptr;
	ffxFunctions ffxApi{};
};
