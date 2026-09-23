#pragma once

#include <cstdint>
#include <memory>

struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;

/**
 * @brief GPU Intel Open Image Denoise integration for path tracing with temporal stabilization.
 *
 * The CommunityShaders plugin runs on D3D11 (translated to Vulkan by DXVK).
 * OIDN's GPU device consumes external Vulkan buffers, so the prepare and
 * composite passes run as D3D11 compute shaders while the plugin fetches the
 * underlying Vulkan buffer of each D3D11 resource and bridges them with the
 * OIDN-owned external buffers using GPU buffer copies.
 *
 * Pipeline (all on the render thread, per frame):
 *   1. D3D11 prepare compute writes linearized color/albedo/normal into packed
 *      fp16 structured buffers.
 *   2. Vulkan copies those buffers into OIDN-owned external Vulkan buffers.
 *   3. OIDN denoises asynchronously, fenced with external timeline semaphores.
 *   4. Vulkan copies the denoised buffer back into a D3D11 structured buffer.
 *   5. D3D11 composite & stabilization pass reprojects previous history using
 *      motion vectors, verifies depth disocclusion, clips against the local
 *      YCoCg variance box, accumulates history, and writes the game main texture
 *      and motion vectors.
 */
class OIDNDenoiser
{
public:
	enum class Quality
	{
		Fast,
		Balanced,
		High
	};

	struct TemporalSettings
	{
		bool enabled = true;
		float historyWeight = 0.95f;
		float depthThreshold = 0.03f;
		int maxAccumulationFrames = 48;

		bool operator==(const TemporalSettings&) const = default;
	};

	OIDNDenoiser();
	~OIDNDenoiser();

	OIDNDenoiser(const OIDNDenoiser&) = delete;
	OIDNDenoiser& operator=(const OIDNDenoiser&) = delete;

	/** @brief Loads OIDN and creates the persistent GPU resources. */
	bool Initialize();

	/** @brief Releases every OIDN/D3D11/Vulkan resource. */
	void Shutdown();

	/** @brief Whether OIDN initialized and the pass can run. */
	bool IsAvailable() const;

	/** @brief Applies the OIDN filter settings. */
	void SetSettings(Quality a_quality, bool a_cleanAux, int a_memoryLimitMB);

	/** @brief Applies temporal stabilization settings. */
	void SetTemporalSettings(const TemporalSettings& a_settings);

	/** @brief Invalidates temporal history (e.g. on cell load or teleport). */
	void ResetHistory();

	/** @brief Exponent used to linearize the display-referred color (1.0 = already linear, 2.2 = gamma). */
	void SetColorDecodeExponent(float a_exponent);

	/**
	 * @brief Runs the full denoise and temporal stabilization pass.
	 * @param a_color          Display-referred color (Creation Engine Raytracing shared main texture).
	 * @param a_albedo         Diffuse albedo output from the raytracing renderer.
	 * @param a_normal         Normal/roughness output from the raytracing renderer.
	 * @param a_motionVectors  Motion vector output from the raytracing renderer.
	 * @param a_depth          Depth output from the raytracing renderer.
	 * @param a_output         Game main texture UAV that receives the denoised result.
	 * @param a_outputMotion   Game motion vector UAV that receives the composited motion vectors.
	 */
	void Denoise(ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_albedo,
		ID3D11ShaderResourceView* a_normal, ID3D11ShaderResourceView* a_motionVectors,
		ID3D11ShaderResourceView* a_depth, ID3D11UnorderedAccessView* a_output,
		ID3D11UnorderedAccessView* a_outputMotion,
		uint32_t a_width, uint32_t a_height);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
