#pragma once

#include <d3d12.h>
#include <dxcapi.h>
#include <winrt/base.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/vector.h>

#pragma warning(push)
#pragma warning(disable: 4515)
#include <rtxgi/ddgi/DDGIVolume.h>
#include <rtxgi/ddgi/gfx/DDGIVolume_D3D12.h>
#pragma warning(pop)

namespace DX12
{
	/**
	 * @class DDGIManager
	 * @brief Manages Dynamic Diffuse Global Illumination using NVIDIA's RTXGI SDK.
	 *
	 * DDGIManager implements a probe-based global illumination system that traces rays from
	 * a 3D grid of probes to capture indirect lighting. The system works in four phases:
	 *
	 * **Phase 1 - Probe Ray Tracing (BeginProbeTracing):**
	 * Traces rays from each probe position in random directions. Each probe traces
	 * `raysPerProbe` rays (default 256) to sample the surrounding environment. Ray hits
	 * record radiance (light color) and distance to the hit point.
	 *
	 * **Phase 2 - Probe Blending (BeginProbeBlending + UpdateProbes):**
	 * The RTXGI SDK's compute shaders blend new ray data into the probe textures using
	 * temporal hysteresis. This smooths lighting changes over time and prevents flickering.
	 * Two textures are updated: irradiance (light color) and distance (visibility).
	 *
	 * **Phase 3 - Screen Sampling (BeginScreenSampling):**
	 * During final scene rendering, shaders sample the probe grid to add indirect lighting
	 * to surfaces. The sampling uses octahedral encoding and trilinear interpolation between
	 * the 8 nearest probes to each surface point.
	 *
	 * **Infinite Scrolling Volume:**
	 * When `scrollingEnabled` is true, the probe volume follows the camera by recycling
	 * probes at the trailing edge to the leading edge. This provides GI coverage for
	 * open-world games without requiring a massive static probe grid.
	 *
	 * **Probe Classification:**
	 * Probes inside geometry are marked as inactive to avoid light leaking. The SDK
	 * analyzes ray hit patterns to detect when a probe is embedded in a wall or floor.
	 *
	 * @note Requires DX12 with DXR 1.0 support and a GPU with ray tracing capabilities.
	 * @note Memory usage scales with probe count: 75x20x75 grid = 112,500 probes.
	 */
	class DDGIManager
	{
	public:
		/**
		 * @struct Settings
		 * @brief Configuration parameters for the DDGI system.
		 *
		 * Settings are divided into categories:
		 * - Grid Configuration: Defines the 3D probe grid dimensions and spacing
		 * - Ray Tracing: Controls ray count and distance
		 * - Quality: Temporal stability and update thresholds
		 * - Bias: Prevents self-shadowing artifacts
		 * - Features: Toggle probe relocation, classification, and scrolling
		 */
		struct Settings
		{
			bool enabled = true;  ///< Master toggle for DDGI system

			// Grid Configuration
			// Skyrim uses ~70 game units per meter. Default 75x20x75 @ 140 unit spacing
			// covers approximately 150m x 40m x 150m (112,500 total probes)
			int probeCounts[3] = { 75, 20, 75 };        ///< Number of probes in X, Y, Z
			float probeSpacing[3] = { 140.0f, 140.0f, 140.0f };  ///< World-space distance between probes

			// Ray Tracing
			int raysPerProbe = 256;           ///< Rays traced per probe per frame (higher = better quality, slower)
			float maxRayDistance = 10000.0f;  ///< Maximum ray travel distance in game units

			// Quality - Temporal Stability
			float hysteresis = 0.97f;          ///< Blend factor for new vs old data (0.97 = 97% old, 3% new)
			float irradianceThreshold = 0.25f; ///< Minimum change to trigger probe update
			float brightnessThreshold = 0.10f; ///< Maximum brightness change per frame

			// Bias - Artifact Prevention
			// Values should be 10-20% of probe spacing to prevent self-shadowing
			float viewBias = 14.0f;    ///< Offset along view direction when sampling
			float normalBias = 14.0f;  ///< Offset along surface normal when sampling

			// Feature Toggles
			bool probeRelocationEnabled = false;     ///< Move probes away from geometry (can cause light bleed)
			bool probeClassificationEnabled = true;  ///< Mark probes inside geometry as inactive
			bool scrollingEnabled = true;            ///< Enable infinite scrolling volume

			// Probe Relocation/Classification Thresholds
			float probeMinFrontfaceDistance = 10.0f;       ///< Minimum distance from surfaces for relocation
			float probeRandomRayBackfaceThreshold = 0.25f; ///< Backface ratio threshold for blending
			float probeFixedRayBackfaceThreshold = 0.25f;  ///< Backface ratio threshold for classification

			// Scrolling
			float scrollForwardBias = 200.0f;  ///< Offset volume center ahead of camera (game units)
			float intensity = 1.0f;            ///< Final GI contribution multiplier (0.0 - 2.0)

			bool showProbes = false;  ///< Debug visualization of probe positions
		};

		DDGIManager() = default;
		~DDGIManager();

		/**
		 * @brief Initializes all DDGI resources and the RTXGI SDK volume.
		 *
		 * Creates probe textures, descriptor heaps, root signatures, and compute PSOs.
		 * Must be called before any other methods. Safe to call multiple times (no-op if
		 * already initialized).
		 *
		 * Resources created:
		 * - ProbeRayData: Stores ray trace results (radiance + distance per ray)
		 * - ProbeIrradiance: Octahedral-encoded diffuse lighting per probe
		 * - ProbeDistance: Octahedral-encoded visibility/distance per probe
		 * - ProbeData: Relocation offsets and classification state
		 * - Constants buffer: DDGIVolumeDescGPUPacked for shaders
		 *
		 * @param device DX12 device with DXR support (ID3D12Device5 or higher)
		 * @param descriptorHeap Main CBV/SRV/UAV heap for GI system
		 * @param descriptorHeapStartIndex Starting slot in heap for DDGI SRVs
		 * @return true if initialization succeeded, false on failure
		 */
		bool Initialize(ID3D12Device5* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorHeapStartIndex);

		/**
		 * @brief Releases all DDGI resources and destroys the SDK volume.
		 *
		 * Safe to call multiple times. After shutdown, Initialize() must be called
		 * again before using the system.
		 */
		void Shutdown();

		/**
		 * @brief Destroys and recreates all resources with current settings.
		 *
		 * Call after changing Settings that affect resource dimensions (probeCounts,
		 * probeSpacing, raysPerProbe). Changes to bias values or feature toggles
		 * do not require reinitialization.
		 */
		void Reinitialize();

		/**
		 * @brief Updates volume position and SDK state for the current frame.
		 *
		 * When scrolling is enabled, calculates the scroll anchor point (camera position
		 * plus forward bias) and tells the SDK to recycle probes as needed. The SDK
		 * tracks scroll offsets internally and handles probe data migration.
		 *
		 * On the first frame after initialization, sets the volume origin to prevent
		 * large initial scroll offsets that would cause all probes to reset.
		 *
		 * @param cameraPosition Current camera world position
		 * @param cameraForward Normalized camera forward direction
		 */
		void Update(const DirectX::XMFLOAT3& cameraPosition, const DirectX::XMFLOAT3& cameraForward);

		/**
		 * @brief Prepares GPU state for the start of a new frame.
		 *
		 * Transitions probe textures from UAV to SRV state if needed. On the first
		 * frame after initialization, textures start in UAV state and need this
		 * transition for safe descriptor table binding.
		 *
		 * @param cmdList Command list for recording barriers
		 */
		void BeginFrame(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Prepares GPU state for Phase 1: Probe Ray Tracing.
		 *
		 * Uploads volume constants (origin, rotation matrix, scroll offsets) to GPU
		 * before tracing begins. This ensures probes use the current frame's random
		 * rotation for ray directions.
		 *
		 * Issues a UAV barrier on ProbeRayData to ensure any previous frame's writes
		 * are complete before the ray generation shader writes new data.
		 *
		 * @param cmdList Command list for recording barriers and copies
		 */
		void BeginProbeTracing(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Prepares GPU state for Phase 2: Probe Blending.
		 *
		 * Transitions probe textures (Irradiance, Distance, Data) from SRV to UAV
		 * state for the SDK's compute shaders to write updated values.
		 *
		 * Issues a UAV barrier on ProbeRayData to ensure ray tracing writes are
		 * complete before blending reads the data.
		 *
		 * Uploads volume constants again (SDK may need fresh data for blending).
		 *
		 * @param cmdList Command list for recording barriers and copies
		 */
		void BeginProbeBlending(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Dispatches SDK compute shaders to blend ray data into probes.
		 *
		 * Calls RTXGI SDK functions to:
		 * 1. UpdateDDGIVolumeProbes: Blends new irradiance and distance data
		 * 2. RelocateDDGIVolumeProbes: Moves probes away from geometry (if enabled)
		 * 3. ClassifyDDGIVolumeProbes: Marks probes as active/inactive (if enabled)
		 *
		 * Relocation and classification run every RELOCATION_INTERVAL frames (10)
		 * to reduce flickering from rapid state changes.
		 *
		 * @param cmdList Command list for compute dispatch
		 */
		void UpdateProbes(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Prepares GPU state for Phase 3: Screen-Space Sampling.
		 *
		 * Transitions probe textures from UAV back to SRV state for sampling
		 * during final scene rendering. Shaders can now safely read probe data
		 * to add indirect lighting to surfaces.
		 *
		 * @param cmdList Command list for recording barriers
		 */
		void BeginScreenSampling(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Completes the frame (currently no-op).
		 *
		 * Probe textures remain in SRV state for the next frame's BeginFrame.
		 * Reserved for future use (e.g., async readback of variability data).
		 *
		 * @param cmdList Command list (unused)
		 */
		void EndFrame(ID3D12GraphicsCommandList4* cmdList);

		/**
		 * @brief Renders debug visualization of probe positions.
		 *
		 * Draws spheres at each probe location, colored by their irradiance value.
		 * Useful for debugging probe placement and coverage issues.
		 *
		 * @param cmdList Command list for compute/draw calls
		 * @param outputTexture Render target for visualization
		 * @param depthTexture Scene depth buffer for occlusion
		 * @param width Output texture width
		 * @param height Output texture height
		 */
		void DispatchProbeVisualization(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* outputTexture,
			ID3D12Resource* depthTexture, uint32_t width, uint32_t height);

		// Resource Accessors - Used by GI sampling shaders
		ID3D12Resource* GetProbeIrradiance() const { return m_probeIrradiance.get(); }
		ID3D12Resource* GetProbeDistance() const { return m_probeDistance.get(); }
		ID3D12Resource* GetProbeData() const { return m_probeData.get(); }
		ID3D12Resource* GetProbeRayData() const { return m_probeRayData.get(); }

		/**
		 * @brief Returns packed volume constants for shader binding.
		 *
		 * The DDGIVolumeDescGPUPacked structure contains all data needed by shaders
		 * to sample the probe grid: origin, spacing, probe counts, scroll offsets,
		 * rotation matrix, encoding parameters, and feature flags.
		 *
		 * @return Packed constants structure, or zero-initialized if not initialized
		 */
		rtxgi::DDGIVolumeDescGPUPacked GetVolumeConstants() const;

		Settings& GetSettings() { return m_settings; }
		const Settings& GetSettings() const { return m_settings; }

		bool IsInitialized() const { return m_initialized; }
		bool IsEnabled() const { return m_initialized && m_settings.enabled; }

		/**
		 * @brief Returns total number of probes in the grid.
		 * @return probeCounts.x * probeCounts.y * probeCounts.z
		 */
		int GetNumProbes() const;

		/**
		 * @brief Returns probe grid dimensions.
		 * @param[out] x Probe count in X axis
		 * @param[out] y Probe count in Y axis
		 * @param[out] z Probe count in Z axis
		 */
		void GetProbeGridDimensions(int& x, int& y, int& z) const;

		/**
		 * @brief Returns dimensions for DispatchRays() call during probe tracing.
		 *
		 * The dispatch dimensions match the ProbeRayData texture layout:
		 * - width: raysPerProbe
		 * - height: probeCounts.x * probeCounts.z (probes per horizontal plane)
		 * - depth: probeCounts.y (number of vertical planes)
		 *
		 * @param[out] width Number of rays (X dimension)
		 * @param[out] height Probes per plane (Y dimension)
		 * @param[out] depth Number of planes (Z dimension)
		 */
		void GetRayDispatchDimensions(uint32_t& width, uint32_t& height, uint32_t& depth) const;

	private:
		/**
		 * @brief Creates all probe-related textures.
		 *
		 * Creates six Texture2DArray resources:
		 * - ProbeRayData: RGBA32F, stores ray radiance (RGB) and hit distance (A)
		 * - ProbeIrradiance: RGBA16F, octahedral-encoded diffuse irradiance, 8x8 texels/probe
		 * - ProbeDistance: RG16F, octahedral-encoded distance, 16x16 texels/probe
		 * - ProbeData: RGBA16F, relocation offsets (XYZ) and classification state (W)
		 * - ProbeVariability: R16F, lighting change rate per probe
		 * - ProbeVariabilityAverage: RG32F, reduction output for adaptive quality
		 *
		 * Also creates a readback buffer for ProbeVariabilityAverage and SRVs in
		 * the main GI descriptor heap for screen-space sampling.
		 *
		 * @return true if all textures created successfully
		 */
		bool CreateProbeTextures();

		/**
		 * @brief Creates GPU and upload buffers for volume constants.
		 *
		 * The constants buffer holds DDGIVolumeDescGPUPacked, uploaded each frame
		 * via an intermediate upload buffer. Size is 256-byte aligned per D3D12
		 * constant buffer requirements.
		 *
		 * @return true if buffers created successfully
		 */
		bool CreateConstantsBuffer();

		/**
		 * @brief Creates all compute pipeline state objects.
		 *
		 * Compiles and creates 8 compute PSOs for SDK operations:
		 * - BlendIrradiance/BlendDistance: Update probe lighting from ray data
		 * - Relocation/RelocationReset: Move probes away from geometry
		 * - Classification/ClassificationReset: Mark probes as active/inactive
		 * - VariabilityReduction/ExtraReduction: Compute lighting stability metrics
		 *
		 * Uses GetCommonDefines() and CompileAndCreatePSO() helpers to reduce
		 * code duplication across the 8 PSOs.
		 *
		 * @return true if all PSOs created successfully
		 */
		bool CreatePipelineStates();

		/**
		 * @brief Creates root signature for SDK compute shaders.
		 *
		 * Uses RTXGI SDK's GetDDGIVolumeRootSignatureDesc() to create the expected
		 * root signature layout:
		 * - Root Parameter 0: 32-bit root constants (b0, space1)
		 * - Root Parameter 1: Descriptor table with 7 ranges (t0, u0-u5, space1)
		 *
		 * @return true if root signature created successfully
		 */
		bool CreateRootSignature();

		/**
		 * @brief Creates RTV descriptor heap for probe textures.
		 *
		 * The SDK may use render target operations internally, so Irradiance and
		 * Distance textures are created with ALLOW_RENDER_TARGET. This heap holds
		 * their RTVs (2 descriptors total).
		 *
		 * @return true if heap and RTVs created successfully
		 */
		bool CreateRTVDescriptorHeap();

		/**
		 * @brief Creates shader-visible descriptor heap for SDK shaders.
		 *
		 * Layout (7 descriptors):
		 * - [0] Constants SRV: Structured buffer view of DDGIVolumeDescGPUPacked
		 * - [1] RayData UAV: Ray trace output
		 * - [2] Irradiance UAV: Probe lighting
		 * - [3] Distance UAV: Probe visibility
		 * - [4] ProbeData UAV: Relocation/classification
		 * - [5] Variability UAV: Lighting stability
		 * - [6] VariabilityAverage UAV: Reduction output
		 *
		 * @return true if heap and descriptors created successfully
		 */
		bool CreateDDGIDescriptorHeap();

		/**
		 * @brief Creates a Texture2DArray resource with specified parameters.
		 *
		 * All DDGI textures are created with UAV access. Irradiance and Distance
		 * also get render target access for SDK compatibility.
		 *
		 * @param width Texture width in texels
		 * @param height Texture height in texels
		 * @param arraySize Number of array slices
		 * @param format DXGI pixel format
		 * @param initialState Initial resource state
		 * @param[out] outResource Created resource
		 * @param debugName Debug name for GPU captures
		 * @param allowRenderTarget Add ALLOW_RENDER_TARGET flag
		 * @return true if resource created successfully
		 */
		bool CreateTexture2DArray(UINT width, UINT height, UINT arraySize, DXGI_FORMAT format,
			D3D12_RESOURCE_STATES initialState, winrt::com_ptr<ID3D12Resource>& outResource,
			const wchar_t* debugName, bool allowRenderTarget = false);

		/**
		 * @brief Helper to create DDGI textures using SDK dimension/format queries.
		 *
		 * Wraps CreateTexture2DArray() by querying the SDK for texture dimensions
		 * and format based on texture type and volume description.
		 *
		 * @param type RTXGI texture type enum (RayData, Irradiance, etc.)
		 * @param formatEnum RTXGI format enum (F32x4, F16x4, etc.)
		 * @param[out] out Created resource
		 * @param name Debug name
		 * @param allowRT Add ALLOW_RENDER_TARGET flag
		 * @return true if resource created successfully
		 */
		bool CreateDDGITexture(rtxgi::EDDGIVolumeTextureType type, rtxgi::EDDGIVolumeTextureFormat formatEnum,
			winrt::com_ptr<ID3D12Resource>& out, const wchar_t* name, bool allowRT = false);

		/**
		 * @brief Helper to compile a shader and create a compute PSO.
		 *
		 * Compiles the shader with DXC using cs_6_5 target, then creates the
		 * pipeline state object.
		 *
		 * @param shaderPath Path to HLSL source file
		 * @param defines Preprocessor defines for compilation
		 * @param entryPoint Shader entry point name
		 * @param[out] outPSO Created pipeline state
		 * @param psoName Debug name for GPU captures
		 * @return true if compilation and PSO creation succeeded
		 */
		bool CompileAndCreatePSO(const wchar_t* shaderPath, const eastl::vector<DxcDefine>& defines,
			const wchar_t* entryPoint, winrt::com_ptr<ID3D12PipelineState>& outPSO, const wchar_t* psoName);

		/**
		 * @brief Returns preprocessor defines common to all DDGI shaders.
		 *
		 * Includes resource management mode, register bindings, coordinate system,
		 * and shared memory settings. Individual PSOs add their specific defines
		 * to this base set.
		 *
		 * @return Vector of DxcDefine structures
		 */
		eastl::vector<DxcDefine> GetCommonDefines() const;

		/**
		 * @brief Snaps a position to the nearest probe grid point.
		 *
		 * Used internally for volume positioning calculations.
		 *
		 * @param position World-space position
		 * @return Snapped position aligned to probe grid
		 */
		DirectX::XMFLOAT3 SnapToProbeGrid(const DirectX::XMFLOAT3& position) const;

		/**
		 * @brief Uploads volume constants to GPU for the current frame.
		 *
		 * Maps the upload buffer, copies packed volume constants, then issues
		 * copy and barrier commands to transfer data to the GPU buffer.
		 *
		 * Tracks upload state to handle first-frame COMMON -> COPY_DEST transition
		 * vs subsequent NON_PIXEL_SHADER_RESOURCE -> COPY_DEST transitions.
		 *
		 * @param cmdList Command list for copy and barrier commands
		 */
		void UploadConstants(ID3D12GraphicsCommandList* cmdList);

	private:
		bool m_initialized = false;
		Settings m_settings;

		ID3D12Device5* m_device = nullptr;

		eastl::unique_ptr<rtxgi::d3d12::DDGIVolume> m_volume;  ///< RTXGI SDK volume object
		rtxgi::DDGIVolumeDesc m_volumeDesc;  ///< Volume configuration passed to SDK

		// Probe Textures
		winrt::com_ptr<ID3D12Resource> m_probeRayData;       ///< Ray trace output (radiance + distance)
		winrt::com_ptr<ID3D12Resource> m_probeIrradiance;    ///< Octahedral diffuse lighting
		winrt::com_ptr<ID3D12Resource> m_probeDistance;      ///< Octahedral visibility
		winrt::com_ptr<ID3D12Resource> m_probeData;          ///< Relocation offsets + classification
		winrt::com_ptr<ID3D12Resource> m_probeVariability;   ///< Lighting change rate
		winrt::com_ptr<ID3D12Resource> m_probeVariabilityAverage;   ///< Reduction output
		winrt::com_ptr<ID3D12Resource> m_probeVariabilityReadback;  ///< CPU readback buffer

		// Constants
		winrt::com_ptr<ID3D12Resource> m_constantsBuffer;        ///< GPU buffer
		winrt::com_ptr<ID3D12Resource> m_constantsBufferUpload;  ///< Upload staging buffer
		UINT64 m_constantsBufferSize = 0;

		// Compute PSOs for SDK operations
		winrt::com_ptr<ID3D12PipelineState> m_blendIrradiancePSO;
		winrt::com_ptr<ID3D12PipelineState> m_blendDistancePSO;
		winrt::com_ptr<ID3D12PipelineState> m_relocationPSO;
		winrt::com_ptr<ID3D12PipelineState> m_relocationResetPSO;
		winrt::com_ptr<ID3D12PipelineState> m_classificationPSO;
		winrt::com_ptr<ID3D12PipelineState> m_classificationResetPSO;
		winrt::com_ptr<ID3D12PipelineState> m_variabilityReductionPSO;
		winrt::com_ptr<ID3D12PipelineState> m_variabilityExtraReductionPSO;
		winrt::com_ptr<ID3D12PipelineState> m_probeVisualizationPSO;

		winrt::com_ptr<ID3D12StateObject> m_probeTraceStateObject;  ///< DXR state object (unused currently)
		winrt::com_ptr<ID3D12RootSignature> m_rootSignature;       ///< SDK compute root signature

		// Descriptor Heaps
		winrt::com_ptr<ID3D12DescriptorHeap> m_rtvDescriptorHeap;   ///< RTVs for Irradiance/Distance
		D3D12_CPU_DESCRIPTOR_HANDLE m_probeIrradianceRTV = {};
		D3D12_CPU_DESCRIPTOR_HANDLE m_probeDistanceRTV = {};

		winrt::com_ptr<ID3D12DescriptorHeap> m_ddgiDescriptorHeap;  ///< UAVs for SDK shaders
		static constexpr UINT DDGI_DESCRIPTOR_COUNT = 7;

		// External descriptor heap (for screen-space sampling)
		ID3D12DescriptorHeap* m_descriptorHeap = nullptr;
		UINT m_descriptorHeapStartIndex = 0;
		UINT m_descriptorSize = 0;

		// Volume State
		DirectX::XMFLOAT3 m_volumeOrigin = { 0, 0, 0 };
		DirectX::XMFLOAT3 m_lastCameraPosition = { 0, 0, 0 };

		uint32_t m_frameCounter = 0;
		static constexpr uint32_t RELOCATION_INTERVAL = 10;  ///< Frames between relocation/classification

		// Resource State Tracking
		bool m_constantsUploaded = false;       ///< Track first upload for state transition
		bool m_probeTexturesInSRVState = false; ///< Track texture states for barriers
		bool m_firstFrame = true;               ///< Track first frame for origin initialization
	};

}  // namespace DX12
