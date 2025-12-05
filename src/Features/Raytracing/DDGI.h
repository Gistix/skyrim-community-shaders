#pragma once

#include <d3d12.h>
#include <winrt/base.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/vector.h>

// RTXGI SDK headers
#pragma warning(push)
#pragma warning(disable: 4515)  // 'namespace uses itself' in RTXGI headers
#include <rtxgi/ddgi/DDGIVolume.h>
#include <rtxgi/ddgi/gfx/DDGIVolume_D3D12.h>
#pragma warning(pop)

namespace DX12
{
	/**
	 * DDGI Manager - Manages NVIDIA RTXGI Dynamic Diffuse Global Illumination
	 *
	 * This class wraps the RTXGI SDK's DDGIVolume and handles:
	 * - Volume creation and configuration
	 * - Probe texture management
	 * - Camera-following scrolling volume updates
	 * - Integration with existing ray tracing pipeline
	 */
	class DDGIManager
	{
	public:
		// Volume configuration settings
		struct Settings
		{
			bool enabled = true;

			// Grid dimensions
			int probeCounts[3] = { 24, 8, 24 };  // X, Y, Z probe counts
			float probeSpacing[3] = { 100.0f, 100.0f, 100.0f };  // World-space spacing in game units

			// Ray tracing settings
			int raysPerProbe = 256;              // Rays traced per probe per frame
			float maxRayDistance = 10000.0f;     // Max ray distance in game units

			// Quality settings
			float hysteresis = 0.97f;            // Temporal stability (0.9-0.99)
			float irradianceThreshold = 0.25f;   // Large lighting change threshold
			float brightnessThreshold = 0.10f;   // Max brightness change per update

			// Bias settings
			float viewBias = 0.3f;               // View ray offset
			float normalBias = 0.1f;             // Normal offset

			// Feature toggles
			bool probeRelocationEnabled = true;  // Move probes away from geometry
			bool probeClassificationEnabled = true;  // Mark inactive probes
			bool scrollingEnabled = true;        // Camera-following volume

			// Scrolling settings
			float scrollForwardBias = 200.0f;    // Forward bias in game units

			// Debug
			bool showProbes = false;
		};

		DDGIManager() = default;
		~DDGIManager();

		// Lifecycle
		bool Initialize(ID3D12Device5* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorHeapStartIndex);
		void Shutdown();

		// Per-frame updates
		void Update(const DirectX::XMFLOAT3& cameraPosition, const DirectX::XMFLOAT3& cameraForward);

		// Probe ray tracing - dispatches rays from probe positions to populate RayData texture
		void TraceProbeRays(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* tlas, ID3D12RootSignature* rootSig);

		// Probe update (blends ray data into irradiance/distance textures)
		void UpdateProbes(ID3D12GraphicsCommandList4* cmdList);

		// Resource transitions
		void TransitionForProbeTrace(ID3D12GraphicsCommandList4* cmdList);
		void TransitionForProbeBlend(ID3D12GraphicsCommandList4* cmdList);
		void TransitionForIrradianceSample(ID3D12GraphicsCommandList4* cmdList);
		void TransitionToUAVForNextFrame(ID3D12GraphicsCommandList4* cmdList);

		// Getters for shader binding
		ID3D12Resource* GetProbeIrradiance() const { return m_probeIrradiance.get(); }
		ID3D12Resource* GetProbeDistance() const { return m_probeDistance.get(); }
		ID3D12Resource* GetProbeData() const { return m_probeData.get(); }
		ID3D12Resource* GetProbeRayData() const { return m_probeRayData.get(); }

		// Get volume constants for shader
		rtxgi::DDGIVolumeDescGPUPacked GetVolumeConstants() const;

		// Settings
		Settings& GetSettings() { return m_settings; }
		const Settings& GetSettings() const { return m_settings; }

		// Status
		bool IsInitialized() const { return m_initialized; }
		bool IsEnabled() const { return m_initialized && m_settings.enabled; }

		// Get volume info
		int GetNumProbes() const;
		void GetProbeGridDimensions(int& x, int& y, int& z) const;
		void GetRayDispatchDimensions(uint32_t& width, uint32_t& height, uint32_t& depth) const;

	private:
		// Create D3D12 resources
		bool CreateProbeTextures();
		bool CreateConstantsBuffer();
		bool CreatePipelineStates();
		bool CreateRootSignature();
		bool CreateRTVDescriptorHeap();
		bool CreateDDGIDescriptorHeap();  // Creates shader-visible heap for DDGI resources

		// Helper to create texture
		bool CreateTexture2DArray(
			UINT width, UINT height, UINT arraySize,
			DXGI_FORMAT format,
			D3D12_RESOURCE_STATES initialState,
			winrt::com_ptr<ID3D12Resource>& outResource,
			const wchar_t* debugName,
			bool allowRenderTarget = false);

		// Snap position to probe grid to avoid temporal swimming
		DirectX::XMFLOAT3 SnapToProbeGrid(const DirectX::XMFLOAT3& position) const;

	private:
		bool m_initialized = false;
		Settings m_settings;

		// D3D12 device reference
		ID3D12Device5* m_device = nullptr;

		// RTXGI Volume
		eastl::unique_ptr<rtxgi::d3d12::DDGIVolume> m_volume;
		rtxgi::DDGIVolumeDesc m_volumeDesc;

		// Probe textures (Unmanaged Resource Mode - we create and manage these)
		winrt::com_ptr<ID3D12Resource> m_probeRayData;       // RGB: radiance, A: hit distance
		winrt::com_ptr<ID3D12Resource> m_probeIrradiance;    // Octahedral encoded irradiance
		winrt::com_ptr<ID3D12Resource> m_probeDistance;      // Mean distance & distance^2
		winrt::com_ptr<ID3D12Resource> m_probeData;          // XYZ: relocation offset, W: classification
		winrt::com_ptr<ID3D12Resource> m_probeVariability;   // Variability tracking
		winrt::com_ptr<ID3D12Resource> m_probeVariabilityAverage;  // Variability average for reduction
		winrt::com_ptr<ID3D12Resource> m_probeVariabilityReadback; // CPU readback for variability

		// Constants buffer for DDGI volume (required by SDK)
		winrt::com_ptr<ID3D12Resource> m_constantsBuffer;       // GPU buffer for DDGIVolumeDescGPUPacked
		winrt::com_ptr<ID3D12Resource> m_constantsBufferUpload; // Upload buffer for staging
		UINT64 m_constantsBufferSize = 0;

		// Pipeline states for probe update compute shaders
		winrt::com_ptr<ID3D12PipelineState> m_blendIrradiancePSO;
		winrt::com_ptr<ID3D12PipelineState> m_blendDistancePSO;
		winrt::com_ptr<ID3D12PipelineState> m_relocationPSO;
		winrt::com_ptr<ID3D12PipelineState> m_relocationResetPSO;
		winrt::com_ptr<ID3D12PipelineState> m_classificationPSO;
		winrt::com_ptr<ID3D12PipelineState> m_classificationResetPSO;
		winrt::com_ptr<ID3D12PipelineState> m_variabilityReductionPSO;
		winrt::com_ptr<ID3D12PipelineState> m_variabilityExtraReductionPSO;

		// Ray tracing state object for probe tracing
		winrt::com_ptr<ID3D12StateObject> m_probeTraceStateObject;

		// Root signature for DDGI compute shaders
		winrt::com_ptr<ID3D12RootSignature> m_rootSignature;

		// RTV descriptor heap for irradiance/distance textures
		winrt::com_ptr<ID3D12DescriptorHeap> m_rtvDescriptorHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE m_probeIrradianceRTV = {};
		D3D12_CPU_DESCRIPTOR_HANDLE m_probeDistanceRTV = {};

		// DDGI resource descriptor heap (shader visible, for compute shaders)
		// Contains: [0]=Constants SRV, [1]=RayData UAV, [2]=Irradiance UAV,
		//           [3]=Distance UAV, [4]=ProbeData UAV, [5]=Variability UAV, [6]=VariabilityAvg UAV
		winrt::com_ptr<ID3D12DescriptorHeap> m_ddgiDescriptorHeap;
		static constexpr UINT DDGI_DESCRIPTOR_COUNT = 7;

		// Descriptor heap info (external heap passed at init, kept for compatibility)
		ID3D12DescriptorHeap* m_descriptorHeap = nullptr;
		UINT m_descriptorHeapStartIndex = 0;
		UINT m_descriptorSize = 0;

		// Volume position tracking
		DirectX::XMFLOAT3 m_volumeOrigin = { 0, 0, 0 };
		DirectX::XMFLOAT3 m_lastCameraPosition = { 0, 0, 0 };

		// Frame counter for probe relocation scheduling
		uint32_t m_frameCounter = 0;
		static constexpr uint32_t RELOCATION_INTERVAL = 10;  // Relocate every N frames
	};

}  // namespace DX12
