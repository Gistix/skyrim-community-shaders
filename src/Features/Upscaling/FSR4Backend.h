#pragma once

#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <winrt/base.h>

#include "D3D12Interop.h"
#include "FFXD3D12Backend.h"
#include <ffx_api/ffx_upscale.h>

/**
 * @brief FidelityFX Super Resolution 4 (FSR 4) ML upscaler coordinator owned by the Upscaling feature.
 */
class FSR4Backend
{
public:
	static FSR4Backend* GetSingleton();

	bool Initialize(
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_displayWidth, uint32_t a_displayHeight);

	void Shutdown();

	[[nodiscard]] bool IsAvailable() const noexcept { return initialized; }

	/**
	 * @brief Evaluates FSR 4 upscaling from render resolution to display resolution.
	 */
	bool Evaluate(
		ID3D11Resource* a_colorIn,
		ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth,
		ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_displayWidth, uint32_t a_displayHeight,
		float a_jitterX, float a_jitterY,
		float a_sharpness);

private:
	FSR4Backend() = default;
	~FSR4Backend();

	FSR4Backend(const FSR4Backend&) = delete;
	FSR4Backend& operator=(const FSR4Backend&) = delete;

	bool initialized = false;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t displayWidth = 0;
	uint32_t displayHeight = 0;

	// Shared NT textures for cross-API handoff
	D3D12Interop::SharedTexture sharedColorIn;
	D3D12Interop::SharedTexture sharedColorOut;
	D3D12Interop::SharedTexture sharedDepth;
	D3D12Interop::SharedTexture sharedMotion;

	ffxContext upscaleContext = nullptr;
};
