#include "FFXD3D12Backend.h"

#include "DxvkLoader.h"
#include "Globals.h"

FFXD3D12Backend* FFXD3D12Backend::GetSingleton()
{
	static FFXD3D12Backend singleton;
	return &singleton;
}

FFXD3D12Backend::~FFXD3D12Backend()
{
	Shutdown();
}

bool FFXD3D12Backend::Initialize(ID3D12Device* a_device)
{
	if (initialized && device == a_device)
		return true;

	if (!a_device) {
		logger::error("[FFXD3D12Backend] Cannot initialize without valid ID3D12Device");
		return false;
	}

	// 1. Locate and dynamically load FidelityFX D3D12 runtime (prefer v2.3+ modular loader)
	if (!ffxModule) {
		const auto runtimeDir = DxvkLoader::GetRuntimeDir();
		if (!runtimeDir.empty()) {
			SetDllDirectoryW(runtimeDir.c_str());
		}

		std::vector<std::filesystem::path> dllCandidates;
		if (!runtimeDir.empty()) {
			dllCandidates.push_back(runtimeDir / L"amd_fidelityfx_loader_dx12.dll");
			dllCandidates.push_back(runtimeDir / L"amd_fidelityfx_upscaler_dx12.dll");
			dllCandidates.push_back(runtimeDir / L"amd_fidelityfx_dx12.dll");
		}
		dllCandidates.push_back(L"Data\\SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_loader_dx12.dll");
		dllCandidates.push_back(L"SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_loader_dx12.dll");
		dllCandidates.push_back(L"amd_fidelityfx_loader_dx12.dll");
		dllCandidates.push_back(L"Data\\SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_upscaler_dx12.dll");
		dllCandidates.push_back(L"SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_upscaler_dx12.dll");
		dllCandidates.push_back(L"amd_fidelityfx_upscaler_dx12.dll");
		dllCandidates.push_back(L"Data\\SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_dx12.dll");
		dllCandidates.push_back(L"SKSE\\Plugins\\CommunityShaders\\bin\\amd_fidelityfx_dx12.dll");
		dllCandidates.push_back(L"amd_fidelityfx_dx12.dll");

		for (const auto& path : dllCandidates) {
			ffxModule = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (ffxModule) {
				logger::info("[FFXD3D12Backend] Loaded FidelityFX D3D12 runtime from '{}'", path.string());
				break;
			}
		}
	}

	if (!ffxModule) {
		logger::warn("[FFXD3D12Backend] FidelityFX D3D12 runtime DLL not found; FidelityFX D3D12 features disabled");
		return false;
	}

	// 2. Resolve exports
	ffxLoadFunctions(&ffxApi, ffxModule);
	if (!ffxApi.CreateContext || !ffxApi.DestroyContext || !ffxApi.Dispatch) {
		logger::error("[FFXD3D12Backend] Failed to resolve required functions from FidelityFX D3D12 runtime DLL");
		FreeLibrary(ffxModule);
		ffxModule = nullptr;
		return false;
	}

	device = a_device;
	initialized = true;
	logger::info("[FFXD3D12Backend] Successfully initialized FidelityFX SDK D3D12 backend loader");
	return true;
}

void FFXD3D12Backend::Shutdown()
{
	device = nullptr;
	initialized = false;

	if (ffxModule) {
		FreeLibrary(ffxModule);
		ffxModule = nullptr;
	}

	ffxApi = {};
	initialized = false;
	logger::info("[FFXD3D12Backend] Shutdown completed");
}

FfxApiResource FFXD3D12Backend::CreateResource(
	const D3D12Interop::SharedTexture& a_texture,
	uint32_t a_state,
	uint32_t a_usage)
{
	return CreateResource(a_texture.d3dResource.get(), a_texture.width, a_texture.height, a_texture.dxgiFormat, a_state, a_usage);
}

FfxApiResource FFXD3D12Backend::CreateResource(
	ID3D12Resource* a_resource,
	uint32_t a_width, uint32_t a_height,
	DXGI_FORMAT a_format,
	uint32_t a_state,
	uint32_t a_usage)
{
	FfxApiResource res{};
	res.resource = a_resource;
	res.description.type = FFX_API_RESOURCE_DIMENSION_TEXTURE_2D;
	res.description.format = ffxApiGetSurfaceFormatDX12(a_format);
	res.description.width = a_width;
	res.description.height = a_height;
	res.description.depth = 1;
	res.description.mipCount = 1;
	res.description.flags = 0;
	res.description.usage = a_usage;
	res.state = a_state;
	return res;
}
