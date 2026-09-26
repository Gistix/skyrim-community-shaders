#pragma once

#include <cstdint>
#include <memory>

struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;

/**
 * @brief FidelityFX Ray Regeneration (MLD / FSR-RR) neural denoiser integration.
 *
 * Implements GPU-accelerated machine learning denoising for path tracing.
 *
 * Pipeline:
 *   1. D3D11 prepare compute shader packs radiance, diffuse albedo, specular albedo,
 *      normals, roughness, motion vectors, linear depth, and hit distance / curvature
 *      into a 16-channel FP16 tensor buffer.
 *   2. DXVK Vulkan interop bridges the buffer to the inference engine.
 *   3. D3D11 composite shader re-modulates the denoised diffuse and specular radiance
 *      with albedo guides, gamma encodes, and blends with the raster sky into the final
 *      render targets.
 */
class FSRRRDenoiser
{
public:
	FSRRRDenoiser();
	~FSRRRDenoiser();

	FSRRRDenoiser(const FSRRRDenoiser&) = delete;
	FSRRRDenoiser& operator=(const FSRRRDenoiser&) = delete;

	/** @brief Initializes the ML inference backend and creates GPU resources. */
	bool Initialize();

	/** @brief Releases all ML runtime, D3D11, and Vulkan resources. */
	void Shutdown();

	/** @brief Whether FSR-RR initialized and the denoiser pass can execute. */
	bool IsAvailable() const;

	/** @brief Exponent used to decode display-referred color (1.0 = linear, 2.2 = sRGB gamma). */
	void SetColorDecodeExponent(float a_exponent);

	/**
	 * @brief Runs the complete FSR-RR denoise and composite pass.
	 * @param a_color          Display-referred path traced radiance.
	 * @param a_diffuseAlbedo  Diffuse albedo output from raytracing renderer (or nullptr fallback).
	 * @param a_specularAlbedo Specular albedo output from raytracing renderer (or nullptr fallback).
	 * @param a_normal         World space normal and roughness.
	 * @param a_motionVectors  Motion vector output from raytracing renderer.
	 * @param a_depth          Depth output from raytracing renderer.
	 * @param a_hitDist        Specular hit distance / AO guide (or nullptr fallback).
	 * @param a_output         Game main texture UAV receiving the denoised result (read/write).
	 * @param a_outputMotion   Game motion vector UAV receiving composited motion vectors.
	 */
	void Denoise(
		ID3D11ShaderResourceView* a_color,
		ID3D11ShaderResourceView* a_diffuseAlbedo,
		ID3D11ShaderResourceView* a_specularAlbedo,
		ID3D11ShaderResourceView* a_normal,
		ID3D11ShaderResourceView* a_motionVectors,
		ID3D11ShaderResourceView* a_depth,
		ID3D11ShaderResourceView* a_hitDist,
		ID3D11UnorderedAccessView* a_output,
		ID3D11UnorderedAccessView* a_outputMotion,
		uint32_t a_width, uint32_t a_height);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
