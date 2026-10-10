#pragma once

#include <cstdint>
#include <d3d11.h>
#include <d3d12.h>
#include <memory>
#include <winrt/base.h>

#include "D3D12Interop.h"
#include "FFXD3D12Backend.h"

#define FFX_DENOISER_VERSION_MAJOR 1
#define FFX_DENOISER_VERSION_MINOR 2
#define FFX_DENOISER_VERSION_PATCH 0
#define FFX_DENOISER_VERSION ((FFX_DENOISER_VERSION_MAJOR << 22) | (FFX_DENOISER_VERSION_MINOR << 12) | FFX_DENOISER_VERSION_PATCH)

#define FFX_API_EFFECT_ID_DENOISER 0x00050000u
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER 0x00050001u
#define FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE 0x00050021u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER 0x00050041u

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION  0x00050043u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE     0x00050044u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR    0x00050045u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT     0x00050046u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE   0x00050047u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR  0x00050048u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION 0x00050049u

enum FfxApiDenoiserSignalFlags : uint32_t
{
	FFX_DENOISER_SIGNAL_NONE                      = 0,
	FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION         = (1 << 0),
	FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE            = (1 << 1),
	FFX_DENOISER_SIGNAL_DIRECT_SPECULAR           = (1 << 2),
	FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY = (1 << 3),
	FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE          = (1 << 4),
	FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR         = (1 << 5),
	FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION        = (1 << 6),
};

enum FfxApiCreateContextDenoiserFlags : uint32_t
{
	FFX_DENOISER_ENABLE_DEBUGGING  = (1 << 0),
	FFX_DENOISER_ENABLE_VALIDATION = (1 << 1),
};

enum FfxApiDispatchDenoiserFlags : uint32_t
{
	FFX_DENOISER_DISPATCH_RESET            = (1 << 0),
	FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO = (1 << 1),
};

struct ffxCreateContextDescDenoiser
{
	ffxCreateContextDescHeader header;
	uint32_t version;
	FfxApiDimensions2D maxRenderSize;
	uint32_t signalFlags;
	uint32_t checkerboardSignalFlags;
	uint32_t flags;
};

struct FfxApiFloatBounds
{
	float min;
	float max;
};

struct FfxApiFloatCoords3D
{
	float x, y, z;
};

struct FfxApiMatrix4x4
{
	float rows[4][4];
};

struct ffxDispatchDescDenoiser
{
	ffxDispatchDescHeader header;
	ID3D12GraphicsCommandList* commandList;
	FfxApiResource linearDepth;
	FfxApiResource motionVectors;
	FfxApiResource normals;
	FfxApiResource diffuseAlbedo;
	FfxApiResource specularAlbedo;
	FfxApiFloatCoords3D motionVectorScale;
	FfxApiFloatCoords2D jitterOffsets;
	FfxApiFloatCoords3D cameraPositionDelta;
	FfxApiMatrix4x4 view;
	FfxApiMatrix4x4 projection;
	FfxApiFloatBounds linearDepthBounds;
	FfxApiDimensions2D renderSize;
	uint32_t flags;
};

struct FfxApiDenoiserSignal
{
	FfxApiResource input;
	FfxApiResource output;
	uint32_t checkerboardOrigin;
};

struct ffxDispatchDescDenoiserDirectDiffuse
{
	ffxDispatchDescHeader header;
	FfxApiDenoiserSignal signal;
};

struct ffxDispatchDescDenoiserDirectSpecular
{
	ffxDispatchDescHeader header;
	FfxApiDenoiserSignal signal;
};

struct ffxDispatchDescDenoiserIndirectDiffuse
{
	ffxDispatchDescHeader header;
	FfxApiDenoiserSignal signal;
};

struct ffxDispatchDescDenoiserIndirectSpecular
{
	ffxDispatchDescHeader header;
	FfxApiDenoiserSignal signal;
};

/**
 * @brief FidelityFX Ray Regeneration (denoiser) coordinator owned by the Upscaling feature.
 */
class FSRRRDenoiser
{
public:
	static FSRRRDenoiser* GetSingleton();

	bool Initialize(uint32_t a_width, uint32_t a_height);
	void Shutdown();

	[[nodiscard]] bool IsAvailable() const noexcept { return initialized; }

	/**
	 * @brief Evaluates FidelityFX Ray Regeneration for path-traced lighting.
	 */
	bool Evaluate(
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
		float a_jitterX, float a_jitterY);

private:
	FSRRRDenoiser() = default;
	~FSRRRDenoiser();

	FSRRRDenoiser(const FSRRRDenoiser&) = delete;
	FSRRRDenoiser& operator=(const FSRRRDenoiser&) = delete;

	bool InitCompositePipeline(ID3D12Device* a_device);

	bool initialized = false;
	uint32_t currentWidth = 0;
	uint32_t currentHeight = 0;

	// Shared NT textures for cross-API handoff
	D3D12Interop::SharedTexture sharedDirectDiffuseIn;
	D3D12Interop::SharedTexture sharedDirectSpecularIn;
	D3D12Interop::SharedTexture sharedIndirectDiffuseIn;
	D3D12Interop::SharedTexture sharedIndirectSpecularIn;

	D3D12Interop::SharedTexture sharedDenoisedDirectDiffuse;
	D3D12Interop::SharedTexture sharedDenoisedDirectSpecular;
	D3D12Interop::SharedTexture sharedDenoisedIndirectDiffuse;
	D3D12Interop::SharedTexture sharedDenoisedIndirectSpecular;

	D3D12Interop::SharedTexture sharedDiffuseAlbedo;
	D3D12Interop::SharedTexture sharedSpecularAlbedo;
	D3D12Interop::SharedTexture sharedNormals;
	D3D12Interop::SharedTexture sharedLinearDepth;
	D3D12Interop::SharedTexture sharedMotion;

	D3D12Interop::SharedTexture sharedEmissiveIn;
	D3D12Interop::SharedTexture sharedFinalOut;

	ffxContext denoiserContext = nullptr;

	// D3D12 composition compute pipeline
	winrt::com_ptr<ID3D12RootSignature> compositeRootSig;
	winrt::com_ptr<ID3D12PipelineState> compositePSO;
	winrt::com_ptr<ID3D12DescriptorHeap> compositeHeap;
};
