#pragma once

#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <winrt/base.h>

#include "D3D12Interop.h"
#include "FFXD3D12Backend.h"

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
		ID3D11Resource* a_colorIn,
		ID3D11Resource* a_diffuseAlbedo,
		ID3D11Resource* a_specularAlbedo,
		ID3D11Resource* a_normalRoughness,
		ID3D11Resource* a_depth,
		ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_specHitDist,
		ID3D11Resource* a_outputMain,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		float a_jitterX, float a_jitterY);

private:
	FSRRRDenoiser() = default;
	~FSRRRDenoiser();

	FSRRRDenoiser(const FSRRRDenoiser&) = delete;
	FSRRRDenoiser& operator=(const FSRRRDenoiser&) = delete;

	bool initialized = false;
	uint32_t currentWidth = 0;
	uint32_t currentHeight = 0;

	// Shared NT textures for cross-API handoff
	D3D12Interop::SharedTexture sharedColorIn;
	D3D12Interop::SharedTexture sharedDenoisedOut;
	D3D12Interop::SharedTexture sharedDepth;
	D3D12Interop::SharedTexture sharedMotion;
	D3D12Interop::SharedTexture sharedNormals;

	ffxContext denoiserContext = nullptr;
};
