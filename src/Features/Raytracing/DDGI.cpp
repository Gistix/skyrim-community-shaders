#include "DDGI.h"
#include "ShaderUtils.h"
#include "Utils/Format.h"
#include <DirectXMath.h>
#include <directx/d3dx12.h>

namespace DX12
{
	DDGIManager::~DDGIManager()
	{
		Shutdown();
	}

	bool DDGIManager::Initialize(ID3D12Device5* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorHeapStartIndex)
	{
		if (m_initialized)
			return true;

		if (!device)
		{
			logger::error("[DDGI] Initialize called with null device");
			return false;
		}

		// Check if device is still valid
		HRESULT deviceStatus = device->GetDeviceRemovedReason();
		if (FAILED(deviceStatus))
		{
			logger::error("[DDGI] Device was already removed at init, reason: 0x{:08X}", static_cast<unsigned int>(deviceStatus));
			return false;
		}

		m_device = device;
		m_descriptorHeap = descriptorHeap;
		m_descriptorHeapStartIndex = descriptorHeapStartIndex;
		m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// Configure volume description
		m_volumeDesc.name = const_cast<char*>("DDGIVolume_Main");
		m_volumeDesc.index = 0;

		// Probe grid configuration
		m_volumeDesc.probeCounts.x = m_settings.probeCounts[0];
		m_volumeDesc.probeCounts.y = m_settings.probeCounts[1];
		m_volumeDesc.probeCounts.z = m_settings.probeCounts[2];

		m_volumeDesc.probeSpacing.x = m_settings.probeSpacing[0];
		m_volumeDesc.probeSpacing.y = m_settings.probeSpacing[1];
		m_volumeDesc.probeSpacing.z = m_settings.probeSpacing[2];

		// Ray tracing settings
		m_volumeDesc.probeNumRays = m_settings.raysPerProbe;
		m_volumeDesc.probeMaxRayDistance = m_settings.maxRayDistance;

		// Probe texture resolution (interior texels, border is added automatically)
		m_volumeDesc.probeNumIrradianceInteriorTexels = 6;  // 6x6 octahedral
		m_volumeDesc.probeNumDistanceInteriorTexels = 14;   // 14x14 for better visibility

		// Compute full texel counts including 1-pixel border
		m_volumeDesc.probeNumIrradianceTexels = m_volumeDesc.probeNumIrradianceInteriorTexels + 2;
		m_volumeDesc.probeNumDistanceTexels = m_volumeDesc.probeNumDistanceInteriorTexels + 2;

		// Quality settings
		m_volumeDesc.probeHysteresis = m_settings.hysteresis;
		m_volumeDesc.probeIrradianceThreshold = m_settings.irradianceThreshold;
		m_volumeDesc.probeBrightnessThreshold = m_settings.brightnessThreshold;
		m_volumeDesc.probeDistanceExponent = 50.0f;
		m_volumeDesc.probeIrradianceEncodingGamma = 5.0f;

		// Bias settings
		m_volumeDesc.probeViewBias = m_settings.viewBias;
		m_volumeDesc.probeNormalBias = m_settings.normalBias;

		// Feature toggles
		m_volumeDesc.probeRelocationEnabled = m_settings.probeRelocationEnabled;
		m_volumeDesc.probeClassificationEnabled = m_settings.probeClassificationEnabled;
		m_volumeDesc.probeVariabilityEnabled = false;  // Optional feature

		// Movement type
		m_volumeDesc.movementType = m_settings.scrollingEnabled
			? rtxgi::EDDGIVolumeMovementType::Scrolling
			: rtxgi::EDDGIVolumeMovementType::Default;

		// Texture formats
		m_volumeDesc.probeRayDataFormat = rtxgi::EDDGIVolumeTextureFormat::F32x4;
		m_volumeDesc.probeIrradianceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
		m_volumeDesc.probeDistanceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x2;
		m_volumeDesc.probeDataFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
		m_volumeDesc.probeVariabilityFormat = rtxgi::EDDGIVolumeTextureFormat::F16;

		// Debug
		m_volumeDesc.showProbes = m_settings.showProbes;

		// Helper lambda to check device status
		auto checkDeviceStatus = [this](const char* stepName) -> bool {
			HRESULT status = m_device->GetDeviceRemovedReason();
			if (FAILED(status)) {
				logger::error("[DDGI] Device removed after {}, reason: 0x{:08X}", stepName, static_cast<unsigned int>(status));
				return false;
			}
			logger::debug("[DDGI] Device OK after {}", stepName);
			return true;
		};

		// Create GPU resources with device checks after each step
		logger::debug("[DDGI] Starting resource creation...");

		if (!CreateProbeTextures())
		{
			logger::error("[DDGI] Failed to create probe textures");
			return false;
		}
		if (!checkDeviceStatus("CreateProbeTextures")) return false;

		if (!CreateConstantsBuffer())
		{
			logger::error("[DDGI] Failed to create constants buffer");
			return false;
		}
		if (!checkDeviceStatus("CreateConstantsBuffer")) return false;

		if (!CreateDDGIDescriptorHeap())
		{
			logger::error("[DDGI] Failed to create DDGI descriptor heap");
			return false;
		}
		if (!checkDeviceStatus("CreateDDGIDescriptorHeap")) return false;

		if (!CreateRTVDescriptorHeap())
		{
			logger::error("[DDGI] Failed to create RTV descriptor heap");
			return false;
		}
		if (!checkDeviceStatus("CreateRTVDescriptorHeap")) return false;

		if (!CreateRootSignature())
		{
			logger::error("[DDGI] Failed to create root signature");
			return false;
		}
		if (!checkDeviceStatus("CreateRootSignature")) return false;

		if (!CreatePipelineStates())
		{
			logger::error("[DDGI] Failed to create pipeline states");
			return false;
		}
		if (!checkDeviceStatus("CreatePipelineStates")) return false;

		// Create RTXGI volume
		m_volume = eastl::make_unique<rtxgi::d3d12::DDGIVolume>();

		// Setup unmanaged resources
		rtxgi::d3d12::DDGIVolumeResources resources = {};
		resources.unmanaged.enabled = true;
		resources.unmanaged.rootSignature = m_rootSignature.get();

		// Texture resources
		resources.unmanaged.probeRayData = m_probeRayData.get();
		resources.unmanaged.probeIrradiance = m_probeIrradiance.get();
		resources.unmanaged.probeDistance = m_probeDistance.get();
		resources.unmanaged.probeData = m_probeData.get();
		resources.unmanaged.probeVariability = m_probeVariability.get();
		resources.unmanaged.probeVariabilityAverage = m_probeVariabilityAverage.get();
		resources.unmanaged.probeVariabilityReadback = m_probeVariabilityReadback.get();

		// RTV descriptors (required by SDK validation)
		resources.unmanaged.probeIrradianceRTV = m_probeIrradianceRTV;
		resources.unmanaged.probeDistanceRTV = m_probeDistanceRTV;

		// Pipeline states
		resources.unmanaged.probeBlendingIrradiancePSO = m_blendIrradiancePSO.get();
		resources.unmanaged.probeBlendingDistancePSO = m_blendDistancePSO.get();
		resources.unmanaged.probeRelocation.updatePSO = m_relocationPSO.get();
		resources.unmanaged.probeRelocation.resetPSO = m_relocationResetPSO.get();
		resources.unmanaged.probeClassification.updatePSO = m_classificationPSO.get();
		resources.unmanaged.probeClassification.resetPSO = m_classificationResetPSO.get();
		resources.unmanaged.probeVariabilityPSOs.reductionPSO = m_variabilityReductionPSO.get();
		resources.unmanaged.probeVariabilityPSOs.extraReductionPSO = m_variabilityExtraReductionPSO.get();

		// Root parameter slot indices (must match root signature created by GetDDGIVolumeRootSignatureDesc)
		// Param 0: Root constants (b0, space1)
		// Param 1: Descriptor table with all resources (t0, u0-u5, space1)
		resources.unmanaged.rootParamSlotRootConstants = 0;
		resources.unmanaged.rootParamSlotResourceDescriptorTable = 1;

		// Constants buffer (required by SDK)
		resources.constantsBuffer = m_constantsBuffer.get();
		resources.constantsBufferUpload = m_constantsBufferUpload.get();
		resources.constantsBufferSizeInBytes = m_constantsBufferSize;

		// Descriptor heap - use our dedicated DDGI descriptor heap
		resources.descriptorHeap.resources = m_ddgiDescriptorHeap.get();
		resources.descriptorHeap.entrySize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		resources.descriptorHeap.constantsIndex = 0;  // Constants SRV at slot 0
		resources.descriptorHeap.resourceIndices.rayDataUAVIndex = 1;
		resources.descriptorHeap.resourceIndices.probeIrradianceUAVIndex = 2;
		resources.descriptorHeap.resourceIndices.probeDistanceUAVIndex = 3;
		resources.descriptorHeap.resourceIndices.probeDataUAVIndex = 4;
		resources.descriptorHeap.resourceIndices.probeVariabilityUAVIndex = 5;
		resources.descriptorHeap.resourceIndices.probeVariabilityAverageUAVIndex = 6;

		// Debug: Log PSO pointers before creating volume
		logger::debug("[DDGI] Creating volume with resources:");
		logger::debug("[DDGI]   BlendIrradiance: {}", m_blendIrradiancePSO ? "valid" : "NULL");
		logger::debug("[DDGI]   BlendDistance: {}", m_blendDistancePSO ? "valid" : "NULL");
		logger::debug("[DDGI]   Relocation: {}", m_relocationPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   RelocationReset: {}", m_relocationResetPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   Classification: {}", m_classificationPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   ClassificationReset: {}", m_classificationResetPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   VariabilityReduction: {}", m_variabilityReductionPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   VariabilityExtraReduction: {}", m_variabilityExtraReductionPSO ? "valid" : "NULL");
		logger::debug("[DDGI]   ConstantsBuffer: {}", m_constantsBuffer ? "valid" : "NULL");
		logger::debug("[DDGI]   ProbeVariability: {}", m_probeVariability ? "valid" : "NULL");
		logger::debug("[DDGI]   ProbeVariabilityAverage: {}", m_probeVariabilityAverage ? "valid" : "NULL");
		logger::debug("[DDGI]   IrradianceRTV: {}", m_probeIrradianceRTV.ptr != 0 ? "valid" : "NULL");
		logger::debug("[DDGI]   DistanceRTV: {}", m_probeDistanceRTV.ptr != 0 ? "valid" : "NULL");

		// Create the volume
		rtxgi::ERTXGIStatus status = m_volume->Create(m_volumeDesc, resources);
		if (status != rtxgi::ERTXGIStatus::OK)
		{
			logger::error("[DDGI] Failed to create DDGI volume, status: {}", static_cast<int>(status));
			return false;
		}

		m_initialized = true;
		logger::info("[DDGI] Initialized with {}x{}x{} probes, spacing ({}, {}, {})",
			m_volumeDesc.probeCounts.x, m_volumeDesc.probeCounts.y, m_volumeDesc.probeCounts.z,
			m_volumeDesc.probeSpacing.x, m_volumeDesc.probeSpacing.y, m_volumeDesc.probeSpacing.z);

		return true;
	}

	void DDGIManager::Shutdown()
	{
		if (!m_initialized)
			return;

		if (m_volume)
		{
			m_volume->Destroy();
			m_volume.reset();
		}

		m_probeRayData = nullptr;
		m_probeIrradiance = nullptr;
		m_probeDistance = nullptr;
		m_probeData = nullptr;
		m_probeVariability = nullptr;
		m_probeVariabilityAverage = nullptr;
		m_probeVariabilityReadback = nullptr;

		m_constantsBuffer = nullptr;
		m_constantsBufferUpload = nullptr;
		m_constantsBufferSize = 0;

		m_rtvDescriptorHeap = nullptr;
		m_probeIrradianceRTV = {};
		m_probeDistanceRTV = {};

		m_ddgiDescriptorHeap = nullptr;

		m_blendIrradiancePSO = nullptr;
		m_blendDistancePSO = nullptr;
		m_relocationPSO = nullptr;
		m_relocationResetPSO = nullptr;
		m_classificationPSO = nullptr;
		m_classificationResetPSO = nullptr;
		m_variabilityReductionPSO = nullptr;
		m_variabilityExtraReductionPSO = nullptr;
		m_rootSignature = nullptr;

		m_initialized = false;
		logger::info("[DDGI] Shutdown complete");
	}

	void DDGIManager::Update(const DirectX::XMFLOAT3& cameraPosition, const DirectX::XMFLOAT3& cameraForward)
	{
		if (!IsEnabled())
			return;

		// Calculate volume center (camera position + forward bias)
		DirectX::XMFLOAT3 volumeCenter;
		volumeCenter.x = cameraPosition.x + cameraForward.x * m_settings.scrollForwardBias;
		volumeCenter.y = cameraPosition.y + cameraForward.y * m_settings.scrollForwardBias;
		volumeCenter.z = cameraPosition.z + cameraForward.z * m_settings.scrollForwardBias;

		// Snap to grid to avoid temporal swimming
		volumeCenter = SnapToProbeGrid(volumeCenter);

		// Calculate volume origin (center - half extents)
		float halfExtentX = m_volumeDesc.probeSpacing.x * (m_volumeDesc.probeCounts.x - 1) * 0.5f;
		float halfExtentY = m_volumeDesc.probeSpacing.y * (m_volumeDesc.probeCounts.y - 1) * 0.5f;
		float halfExtentZ = m_volumeDesc.probeSpacing.z * (m_volumeDesc.probeCounts.z - 1) * 0.5f;

		rtxgi::float3 origin;
		origin.x = volumeCenter.x - halfExtentX;
		origin.y = volumeCenter.y - halfExtentY;
		origin.z = volumeCenter.z - halfExtentZ;

		// Update volume
		if (m_settings.scrollingEnabled)
		{
			// For scrolling volumes, set the scroll anchor
			m_volume->SetScrollAnchor(origin);
		}
		else
		{
			// For default volumes, set origin directly
			m_volume->SetOrigin(origin);
		}

		// Update the volume (computes rotation matrices, scrolling, etc.)
		m_volume->Update();

		m_volumeOrigin = { origin.x, origin.y, origin.z };
		m_lastCameraPosition = cameraPosition;
		m_frameCounter++;
	}

	void DDGIManager::TraceProbeRays([[maybe_unused]] ID3D12GraphicsCommandList4* cmdList, [[maybe_unused]] ID3D12Resource* tlas, [[maybe_unused]] ID3D12RootSignature* rootSig)
	{
		if (!IsEnabled())
			return;

		// Get number of probes and rays for logging
		int numProbes = GetNumProbes();
		int raysPerProbe = m_settings.raysPerProbe;

		logger::debug("[DDGI] TraceProbeRays called: {} probes x {} rays = {} total rays",
			numProbes, raysPerProbe, numProbes * raysPerProbe);

		// Note: The actual DispatchRays call is done in Raytracing::DrawRTGI()
		// because it has access to the full pipeline state object and shader binding table.
		// This method is called for any DDGI-specific setup before the dispatch.
	}

	void DDGIManager::UpdateProbes(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		// Update probe irradiance and distance from ray data
		rtxgi::d3d12::DDGIVolume* volumes[] = { m_volume.get() };
		rtxgi::d3d12::UpdateDDGIVolumeProbes(cmdList, 1, volumes);

		// Relocate probes periodically
		if (m_settings.probeRelocationEnabled && (m_frameCounter % RELOCATION_INTERVAL == 0))
		{
			rtxgi::d3d12::RelocateDDGIVolumeProbes(cmdList, 1, volumes);
		}

		// Classify probes
		if (m_settings.probeClassificationEnabled)
		{
			rtxgi::d3d12::ClassifyDDGIVolumeProbes(cmdList, 1, volumes);
		}
	}

	void DDGIManager::TransitionForProbeTrace(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		// Before probe tracing: transition RayData to UAV state
		// Probe textures stay in UAV (they were created in UAV state)
		// The SDK's POST_PROBE_TRACE expects NON_PIXEL_SHADER_RESOURCE -> UAV
		// but we start in UAV, so we just ensure RayData is ready for writing

		// RayData needs to be in UAV for the ray generation shader to write
		// It's created in UAV state, so no transition needed on first frame
		// After the first frame, it will be in the state we left it

		// For now, just ensure all probe textures are in UAV for tracing
		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeRayData.get(),
				D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		// Skip transition if already in correct state - use UAV barrier instead
		CD3DX12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_probeRayData.get());
		cmdList->ResourceBarrier(1, &uavBarrier);
	}

	void DDGIManager::TransitionForProbeBlend(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		// After probe tracing, before blending:
		// - RayData needs to be readable (NON_PIXEL_SHADER_RESOURCE)
		// - Irradiance/Distance/Data need to be UAV for compute shader writes

		// RayData: UAV -> SRV for reading in blend shaders
		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeRayData.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		};
		cmdList->ResourceBarrier(_countof(barriers), barriers);

		// Probe textures are already in UAV state (created that way or from previous frame)
		// Use UAV barriers to ensure writes are complete
		CD3DX12_RESOURCE_BARRIER uavBarriers[] = {
			CD3DX12_RESOURCE_BARRIER::UAV(m_probeIrradiance.get()),
			CD3DX12_RESOURCE_BARRIER::UAV(m_probeDistance.get()),
			CD3DX12_RESOURCE_BARRIER::UAV(m_probeData.get()),
		};
		cmdList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
	}

	void DDGIManager::TransitionForIrradianceSample(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		// After blending, prepare for sampling in the final GI pass:
		// - Irradiance/Distance/Data need to be readable
		// We use NON_PIXEL_SHADER_RESOURCE because we sample in compute/ray shaders

		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeIrradiance.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeDistance.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeData.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		};
		cmdList->ResourceBarrier(_countof(barriers), barriers);
	}

	void DDGIManager::TransitionToUAVForNextFrame(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		// At end of frame, transition probe textures back to UAV for the next frame's blend operations
		// RayData stays in NON_PIXEL_SHADER_RESOURCE until next probe trace

		CD3DX12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeIrradiance.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeDistance.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeData.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(m_probeRayData.get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cmdList->ResourceBarrier(_countof(barriers), barriers);
	}

	rtxgi::DDGIVolumeDescGPUPacked DDGIManager::GetVolumeConstants() const
	{
		if (m_volume)
			return m_volume->GetDescGPUPacked();
		return {};
	}

	int DDGIManager::GetNumProbes() const
	{
		if (m_volume)
			return m_volume->GetNumProbes();
		return 0;
	}

	void DDGIManager::GetProbeGridDimensions(int& x, int& y, int& z) const
	{
		x = m_volumeDesc.probeCounts.x;
		y = m_volumeDesc.probeCounts.y;
		z = m_volumeDesc.probeCounts.z;
	}

	void DDGIManager::GetRayDispatchDimensions(uint32_t& width, uint32_t& height, uint32_t& depth) const
	{
		if (m_volume)
		{
			m_volume->GetRayDispatchDimensions(width, height, depth);
		}
		else
		{
			// Fallback: calculate manually matching SDK logic
			// For RayData texture: width = numRays, height = probesX * probesZ, depth = probesY
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::RayData, width, height, depth);
		}
	}

	bool DDGIManager::CreateProbeTextures()
	{
		uint32_t probeCountX, probeCountY, probeCountZ;
		rtxgi::GetDDGIVolumeProbeCounts(m_volumeDesc, probeCountX, probeCountY, probeCountZ);

		logger::debug("[DDGI] CreateProbeTextures: Probe grid = {}x{}x{} = {} probes",
			probeCountX, probeCountY, probeCountZ, probeCountX * probeCountY * probeCountZ);
		logger::debug("[DDGI] CreateProbeTextures: Rays per probe = {}", m_volumeDesc.probeNumRays);
		logger::debug("[DDGI] CreateProbeTextures: Irradiance texels = {}, Distance texels = {}",
			m_volumeDesc.probeNumIrradianceTexels, m_volumeDesc.probeNumDistanceTexels);

		// Ray Data texture
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::RayData, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::RayData, m_volumeDesc.probeRayDataFormat);

			logger::debug("[DDGI] Creating RayData texture: {}x{}x{}, format={}", width, height, arraySize, static_cast<int>(format));
			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeRayData, L"DDGI_ProbeRayData"))
				return false;
			logger::debug("[DDGI] RayData texture created successfully");
		}

		// Irradiance texture (needs RTV for SDK)
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Irradiance, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeIrradiance, L"DDGI_ProbeIrradiance", true))
				return false;
		}

		// Distance texture (needs RTV for SDK)
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Distance, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeDistance, L"DDGI_ProbeDistance", true))
				return false;
		}

		// Data texture (relocation + classification)
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Data, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Data, m_volumeDesc.probeDataFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeData, L"DDGI_ProbeData"))
				return false;
		}

		// Variability texture (required by SDK even if disabled)
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Variability, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Variability, m_volumeDesc.probeVariabilityFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeVariability, L"DDGI_ProbeVariability"))
				return false;
		}

		// Variability Average texture (R32G32_FLOAT for reduction)
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::VariabilityAverage, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::VariabilityAverage, rtxgi::EDDGIVolumeTextureFormat::F32x2);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeVariabilityAverage, L"DDGI_ProbeVariabilityAverage"))
				return false;
		}

		// Variability Readback buffer (CPU-accessible for reading back average variability)
		{
			// Get dimensions of variability average texture
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::VariabilityAverage, width, height, arraySize);

			// Readback buffer size: one float2 per reduction result
			UINT64 bufferSize = sizeof(float) * 2 * arraySize;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = bufferSize;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_READBACK;

			HRESULT hr = m_device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(m_probeVariabilityReadback.put()));

			if (FAILED(hr))
			{
				logger::error("[DDGI] Failed to create variability readback buffer");
				return false;
			}
			m_probeVariabilityReadback->SetName(L"DDGI_ProbeVariabilityReadback");
		}

		return true;
	}

	bool DDGIManager::CreateConstantsBuffer()
	{
		// Calculate buffer size for one volume's constants (DDGIVolumeDescGPUPacked)
		// The SDK expects this to be a structured buffer that can hold constants for all volumes
		// We only have one volume, so size = sizeof(DDGIVolumeDescGPUPacked)
		m_constantsBufferSize = sizeof(rtxgi::DDGIVolumeDescGPUPacked);

		// Align to 256 bytes (D3D12 constant buffer alignment requirement)
		m_constantsBufferSize = (m_constantsBufferSize + 255) & ~255ULL;

		// Create GPU buffer (default heap)
		{
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = m_constantsBufferSize;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			HRESULT hr = m_device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_COMMON,
				nullptr,
				IID_PPV_ARGS(m_constantsBuffer.put()));

			if (FAILED(hr))
			{
				logger::error("[DDGI] Failed to create constants buffer (GPU), HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_constantsBuffer->SetName(L"DDGI_ConstantsBuffer");
		}

		// Create upload buffer (upload heap for CPU->GPU transfer)
		{
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = m_constantsBufferSize;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			HRESULT hr = m_device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(m_constantsBufferUpload.put()));

			if (FAILED(hr))
			{
				logger::error("[DDGI] Failed to create constants upload buffer, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_constantsBufferUpload->SetName(L"DDGI_ConstantsBufferUpload");
		}

		logger::debug("[DDGI] Constants buffer created, size: {} bytes", m_constantsBufferSize);
		return true;
	}

	bool DDGIManager::CreateRTVDescriptorHeap()
	{
		// Create RTV descriptor heap for irradiance and distance textures
		// The SDK needs these for potential render target operations
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = 2;  // One for irradiance, one for distance
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // RTV heaps are not shader visible

		HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_rtvDescriptorHeap.put()));
		if (FAILED(hr))
		{
			logger::error("[DDGI] Failed to create RTV descriptor heap, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
			return false;
		}
		m_rtvDescriptorHeap->SetName(L"DDGI_RTVDescriptorHeap");

		UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

		// Create RTV for irradiance texture
		{
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat);
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtvDesc.Texture2DArray.MipSlice = 0;
			rtvDesc.Texture2DArray.FirstArraySlice = 0;
			rtvDesc.Texture2DArray.ArraySize = 1;  // First slice
			rtvDesc.Texture2DArray.PlaneSlice = 0;

			m_device->CreateRenderTargetView(m_probeIrradiance.get(), &rtvDesc, rtvHandle);
			m_probeIrradianceRTV = rtvHandle;
			rtvHandle.ptr += rtvDescriptorSize;
		}

		// Create RTV for distance texture
		{
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat);
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtvDesc.Texture2DArray.MipSlice = 0;
			rtvDesc.Texture2DArray.FirstArraySlice = 0;
			rtvDesc.Texture2DArray.ArraySize = 1;  // First slice
			rtvDesc.Texture2DArray.PlaneSlice = 0;

			m_device->CreateRenderTargetView(m_probeDistance.get(), &rtvDesc, rtvHandle);
			m_probeDistanceRTV = rtvHandle;
		}

		logger::debug("[DDGI] RTV descriptor heap created successfully");
		return true;
	}

	bool DDGIManager::CreateDDGIDescriptorHeap()
	{
		// Create shader-visible descriptor heap for DDGI resources
		// Layout: [0]=Constants SRV, [1]=RayData UAV, [2]=Irradiance UAV,
		//         [3]=Distance UAV, [4]=ProbeData UAV, [5]=Variability UAV, [6]=VariabilityAvg UAV
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = DDGI_DESCRIPTOR_COUNT;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_ddgiDescriptorHeap.put()));
		if (FAILED(hr))
		{
			logger::error("[DDGI] Failed to create DDGI descriptor heap, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
			return false;
		}
		m_ddgiDescriptorHeap->SetName(L"DDGI_ResourceDescriptorHeap");

		UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_ddgiDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

		// [0] Constants SRV - structured buffer view of DDGIVolumeDescGPUPacked
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = 1;
			srvDesc.Buffer.StructureByteStride = sizeof(rtxgi::DDGIVolumeDescGPUPacked);
			srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

			m_device->CreateShaderResourceView(m_constantsBuffer.get(), &srvDesc, cpuHandle);
			cpuHandle.ptr += descriptorSize;
		}

		// Helper to create UAV for Texture2DArray
		auto createTexture2DArrayUAV = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = 0;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = resource->GetDesc().DepthOrArraySize;
			uavDesc.Texture2DArray.PlaneSlice = 0;

			m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, cpuHandle);
			cpuHandle.ptr += descriptorSize;
		};

		// [1] RayData UAV
		createTexture2DArrayUAV(m_probeRayData.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::RayData, m_volumeDesc.probeRayDataFormat));

		// [2] Irradiance UAV
		createTexture2DArrayUAV(m_probeIrradiance.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat));

		// [3] Distance UAV
		createTexture2DArrayUAV(m_probeDistance.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat));

		// [4] ProbeData UAV
		createTexture2DArrayUAV(m_probeData.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Data, m_volumeDesc.probeDataFormat));

		// [5] Variability UAV
		createTexture2DArrayUAV(m_probeVariability.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Variability, m_volumeDesc.probeVariabilityFormat));

		// [6] VariabilityAverage UAV
		createTexture2DArrayUAV(m_probeVariabilityAverage.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::VariabilityAverage, rtxgi::EDDGIVolumeTextureFormat::F32x2));

		logger::debug("[DDGI] DDGI descriptor heap created with {} descriptors", DDGI_DESCRIPTOR_COUNT);
		return true;
	}

	bool DDGIManager::CreateTexture2DArray(
		UINT width, UINT height, UINT arraySize,
		DXGI_FORMAT format,
		D3D12_RESOURCE_STATES initialState,
		winrt::com_ptr<ID3D12Resource>& outResource,
		const wchar_t* debugName,
		bool allowRenderTarget)
	{
		// Validate dimensions
		if (width == 0 || height == 0 || arraySize == 0)
		{
			logger::error("[DDGI] Invalid texture dimensions: {}x{}x{} for {}", width, height, arraySize, Util::WStringToString(debugName));
			return false;
		}

		logger::debug("[DDGI] Creating texture: {} ({}x{}x{}, format: {}, RT: {})",
			Util::WStringToString(debugName), width, height, arraySize, static_cast<int>(format), allowRenderTarget);

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = static_cast<UINT16>(arraySize);
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (allowRenderTarget)
			desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

		HRESULT hr = m_device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			initialState,
			nullptr,
			IID_PPV_ARGS(outResource.put()));

		if (FAILED(hr))
		{
			logger::error("[DDGI] Failed to create texture {}, HRESULT: 0x{:08X}", Util::WStringToString(debugName), static_cast<unsigned int>(hr));
			return false;
		}

		outResource->SetName(debugName);
		logger::debug("[DDGI] Successfully created texture: {}", Util::WStringToString(debugName));
		return true;
	}

	bool DDGIManager::CreateRootSignature()
	{
		// Check if device is still valid before creating root signature
		HRESULT deviceStatus = m_device->GetDeviceRemovedReason();
		if (FAILED(deviceStatus))
		{
			logger::error("[DDGI] Device was removed before root signature creation, reason: 0x{:08X}", static_cast<unsigned int>(deviceStatus));
			return false;
		}

		// Use the SDK's GetDDGIVolumeRootSignatureDesc to create the expected root signature
		// This creates:
		// - Root Parameter 0: 32-bit constants (b0, space1)
		// - Root Parameter 1: Descriptor table with 7 ranges (t0, u0-u5, all in space1)

		// Setup descriptor heap indices - we'll create descriptors at these offsets
		// in our dedicated DDGI descriptor heap
		rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc heapDesc = {};
		heapDesc.constantsIndex = 0;                              // t0: Constants SRV
		heapDesc.resourceIndices.rayDataUAVIndex = 1;             // u0: RayData UAV
		heapDesc.resourceIndices.probeIrradianceUAVIndex = 2;     // u1: Irradiance UAV
		heapDesc.resourceIndices.probeDistanceUAVIndex = 3;       // u2: Distance UAV
		heapDesc.resourceIndices.probeDataUAVIndex = 4;           // u3: ProbeData UAV
		heapDesc.resourceIndices.probeVariabilityUAVIndex = 5;    // u4: Variability UAV
		heapDesc.resourceIndices.probeVariabilityAverageUAVIndex = 6;  // u5: VariabilityAverage UAV

		ID3DBlob* signatureBlob = nullptr;
		if (!rtxgi::d3d12::GetDDGIVolumeRootSignatureDesc(heapDesc, signatureBlob))
		{
			logger::error("[DDGI] Failed to get root signature desc from SDK");
			return false;
		}

		HRESULT hr = m_device->CreateRootSignature(
			0,
			signatureBlob->GetBufferPointer(),
			signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(m_rootSignature.put()));

		signatureBlob->Release();

		if (FAILED(hr))
		{
			logger::error("[DDGI] Failed to create root signature, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
			return false;
		}

		logger::debug("[DDGI] Root signature created successfully using SDK layout");
		return true;
	}

	bool DDGIManager::CreatePipelineStates()
	{
		// Shader path base
		const wchar_t* shaderBasePath = L"Data/Shaders/Raytracing/DDGI/";

		// Common defines for all DDGI shaders
		// Using unmanaged resources (we create textures, SDK doesn't)
		// Using bound resources (not bindless)
		// Using shared memory for better performance

		auto createCommonDefines = [this]() -> eastl::vector<DxcDefine> {
			eastl::vector<DxcDefine> defines;

			// Mark as HLSL to exclude C++ code from SDK headers
			defines.push_back({ L"HLSL", L"1" });

			// Resource management mode: 0 = unmanaged (we create resources)
			defines.push_back({ L"RTXGI_DDGI_RESOURCE_MANAGEMENT", L"0" });

			// No shader reflection - we specify registers explicitly
			defines.push_back({ L"RTXGI_DDGI_SHADER_REFLECTION", L"0" });

			// Not using bindless resources
			defines.push_back({ L"RTXGI_DDGI_BINDLESS_RESOURCES", L"0" });

			// Use shared memory for performance
			defines.push_back({ L"RTXGI_DDGI_BLEND_SHARED_MEMORY", L"1" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY", L"1" });

			// Coordinate system: Left-handed Y-up (Skyrim)
			defines.push_back({ L"RTXGI_COORDINATE_SYSTEM", L"0" });

			// Register bindings for unmanaged mode - use space1 to match SDK root signature
			// The SDK's GetDDGIVolumeRootSignatureDesc uses space1 for all resources
			defines.push_back({ L"VOLUME_CONSTS_REGISTER", L"t0" });
			defines.push_back({ L"VOLUME_CONSTS_SPACE", L"space1" });
			defines.push_back({ L"RAY_DATA_REGISTER", L"u0" });
			defines.push_back({ L"RAY_DATA_SPACE", L"space1" });
			defines.push_back({ L"PROBE_DATA_REGISTER", L"u3" });
			defines.push_back({ L"PROBE_DATA_SPACE", L"space1" });

			// Root constants register (required by DDGIRootConstants.hlsl) - also space1
			defines.push_back({ L"CONSTS_REGISTER", L"b0" });
			defines.push_back({ L"CONSTS_SPACE", L"space1" });

			return defines;
		};

		// Compile ProbeBlendingCS for Irradiance
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"1" });  // Blending irradiance
			defines.push_back({ L"OUTPUT_REGISTER", L"u1" });
			defines.push_back({ L"OUTPUT_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });

			// Probe texel counts (irradiance uses 6x6 interior + 1 border = 8x8)
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_TEXELS", L"8" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_RAYS_PER_PROBE", L"256" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeBlendingCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeBlendingCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ProbeBlendingCS (Irradiance)");
				return false;
			}

			logger::debug("[DDGI] ProbeBlendingCS (Irradiance) compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_blendIrradiancePSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create irradiance blending PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_blendIrradiancePSO->SetName(L"DDGI_BlendIrradiancePSO");
		}

		// Compile ProbeBlendingCS for Distance
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"0" });  // Blending distance
			defines.push_back({ L"OUTPUT_REGISTER", L"u2" });  // Distance uses u2
			defines.push_back({ L"OUTPUT_SPACE", L"space1" });

			// Probe texel counts (distance uses 14x14 interior + 1 border = 16x16)
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_TEXELS", L"16" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"14" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_RAYS_PER_PROBE", L"256" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeBlendingCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeBlendingCS");

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeBlendingCS (Distance)");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_blendDistancePSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create distance blending PSO");
				return false;
			}
			m_blendDistancePSO->SetName(L"DDGI_BlendDistancePSO");
		}

		// Compile ProbeRelocationCS (Update)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_STATE_CLASSIFIER", L"0" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeRelocationCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeRelocationCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ProbeRelocationCS");
				return false;
			}

			logger::debug("[DDGI] ProbeRelocationCS compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_relocationPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create relocation PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_relocationPSO->SetName(L"DDGI_RelocationPSO");
			logger::debug("[DDGI] Relocation PSO created successfully");
		}

		// Compile ProbeRelocationCS (Reset)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION_RESET", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_STATE_CLASSIFIER", L"0" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeRelocationCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeRelocationCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ProbeRelocationCS (Reset)");
				return false;
			}

			logger::debug("[DDGI] ProbeRelocationCS (Reset) compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_relocationResetPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create relocation reset PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_relocationResetPSO->SetName(L"DDGI_RelocationResetPSO");
			logger::debug("[DDGI] Relocation Reset PSO created successfully");
		}

		// Compile ProbeClassificationCS (Update)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION", L"1" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeClassificationCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeClassificationCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ProbeClassificationCS");
				return false;
			}

			logger::debug("[DDGI] ProbeClassificationCS compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_classificationPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create classification PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_classificationPSO->SetName(L"DDGI_ClassificationPSO");
		}

		// Compile ProbeClassificationCS (Reset)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION_RESET", L"1" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeClassificationCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeClassificationCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ProbeClassificationCS (Reset)");
				return false;
			}

			logger::debug("[DDGI] ProbeClassificationCS (Reset) compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_classificationResetPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create classification reset PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_classificationResetPSO->SetName(L"DDGI_ClassificationResetPSO");
			logger::debug("[DDGI] Classification Reset PSO created successfully");
		}

		// Compile ReductionCS (Variability Reduction)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_WAVE_LANE_COUNT", L"32" });  // Assume warp size of 32
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_REGISTER", L"u5" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_SPACE", L"space1" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ReductionCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIReductionCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ReductionCS");
				return false;
			}

			logger::debug("[DDGI] ReductionCS compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_variabilityReductionPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create variability reduction PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_variabilityReductionPSO->SetName(L"DDGI_VariabilityReductionPSO");
			logger::debug("[DDGI] Variability Reduction PSO created successfully");
		}

		// Compile ReductionCS (Extra Reduction)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_WAVE_LANE_COUNT", L"32" });  // Assume warp size of 32
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_REGISTER", L"u5" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_SPACE", L"space1" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ReductionCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIExtraReductionCS");

			if (!shaderBlob || shaderBlob->GetBufferSize() == 0) {
				logger::error("[DDGI] Failed to compile ReductionCS (Extra)");
				return false;
			}

			logger::debug("[DDGI] ReductionCS (Extra) compiled, size: {} bytes", shaderBlob->GetBufferSize());

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_variabilityExtraReductionPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create extra variability reduction PSO, HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
				return false;
			}
			m_variabilityExtraReductionPSO->SetName(L"DDGI_VariabilityExtraReductionPSO");
			logger::debug("[DDGI] Variability Extra Reduction PSO created successfully");
		}

		logger::info("[DDGI] All compute pipeline states created successfully");
		return true;
	}

	DirectX::XMFLOAT3 DDGIManager::SnapToProbeGrid(const DirectX::XMFLOAT3& position) const
	{
		DirectX::XMFLOAT3 snapped;
		snapped.x = std::floor(position.x / m_volumeDesc.probeSpacing.x) * m_volumeDesc.probeSpacing.x;
		snapped.y = std::floor(position.y / m_volumeDesc.probeSpacing.y) * m_volumeDesc.probeSpacing.y;
		snapped.z = std::floor(position.z / m_volumeDesc.probeSpacing.z) * m_volumeDesc.probeSpacing.z;
		return snapped;
	}

}  // namespace DX12
