#include "DDGI.h"
#include "ShaderUtils.h"
#include <DirectXMath.h>

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

		// Create GPU resources
		if (!CreateProbeTextures())
		{
			logger::error("[DDGI] Failed to create probe textures");
			return false;
		}

		if (!CreateRootSignature())
		{
			logger::error("[DDGI] Failed to create root signature");
			return false;
		}

		if (!CreatePipelineStates())
		{
			logger::error("[DDGI] Failed to create pipeline states");
			return false;
		}

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

		// Pipeline states
		resources.unmanaged.probeBlendingIrradiancePSO = m_blendIrradiancePSO.get();
		resources.unmanaged.probeBlendingDistancePSO = m_blendDistancePSO.get();
		resources.unmanaged.probeRelocation.updatePSO = m_relocationPSO.get();
		resources.unmanaged.probeRelocation.resetPSO = m_relocationResetPSO.get();
		resources.unmanaged.probeClassification.updatePSO = m_classificationPSO.get();
		resources.unmanaged.probeClassification.resetPSO = m_classificationResetPSO.get();

		// Descriptor heap
		resources.descriptorHeap.resources = m_descriptorHeap;
		resources.descriptorHeap.entrySize = m_descriptorSize;

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

		m_blendIrradiancePSO = nullptr;
		m_blendDistancePSO = nullptr;
		m_relocationPSO = nullptr;
		m_relocationResetPSO = nullptr;
		m_classificationPSO = nullptr;
		m_classificationResetPSO = nullptr;
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

		m_volume->TransitionResources(cmdList, rtxgi::d3d12::EDDGIExecutionStage::POST_PROBE_TRACE);
	}

	void DDGIManager::TransitionForProbeBlend(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		m_volume->TransitionResources(cmdList, rtxgi::d3d12::EDDGIExecutionStage::PRE_GATHER_CS);
	}

	void DDGIManager::TransitionForIrradianceSample(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		m_volume->TransitionResources(cmdList, rtxgi::d3d12::EDDGIExecutionStage::PRE_GATHER_PS);
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

	bool DDGIManager::CreateProbeTextures()
	{
		uint32_t probeCountX, probeCountY, probeCountZ;
		rtxgi::GetDDGIVolumeProbeCounts(m_volumeDesc, probeCountX, probeCountY, probeCountZ);

		// Ray Data texture
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::RayData, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::RayData, m_volumeDesc.probeRayDataFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeRayData, L"DDGI_ProbeRayData"))
				return false;
		}

		// Irradiance texture
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Irradiance, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeIrradiance, L"DDGI_ProbeIrradiance"))
				return false;
		}

		// Distance texture
		{
			uint32_t width, height, arraySize;
			rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, rtxgi::EDDGIVolumeTextureType::Distance, width, height, arraySize);
			DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat);

			if (!CreateTexture2DArray(width, height, arraySize, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_probeDistance, L"DDGI_ProbeDistance"))
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

		return true;
	}

	bool DDGIManager::CreateTexture2DArray(
		UINT width, UINT height, UINT arraySize,
		DXGI_FORMAT format,
		D3D12_RESOURCE_STATES initialState,
		winrt::com_ptr<ID3D12Resource>& outResource,
		const wchar_t* debugName)
	{
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
			logger::error("[DDGI] Failed to create texture");
			return false;
		}

		outResource->SetName(debugName);
		return true;
	}

	bool DDGIManager::CreateRootSignature()
	{
		// Get the root signature descriptor from RTXGI SDK
		rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc heapDesc = {};
		heapDesc.resources = m_descriptorHeap;
		heapDesc.entrySize = m_descriptorSize;

		ID3DBlob* signatureBlob = nullptr;
		if (!rtxgi::d3d12::GetDDGIVolumeRootSignatureDesc(heapDesc, signatureBlob))
		{
			logger::error("[DDGI] Failed to get root signature descriptor");
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
			logger::error("[DDGI] Failed to create root signature");
			return false;
		}

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

			// Register bindings for unmanaged mode
			defines.push_back({ L"VOLUME_CONSTS_REGISTER", L"t0" });
			defines.push_back({ L"VOLUME_CONSTS_SPACE", L"space0" });
			defines.push_back({ L"RAY_DATA_REGISTER", L"u0" });
			defines.push_back({ L"RAY_DATA_SPACE", L"space0" });
			defines.push_back({ L"PROBE_DATA_REGISTER", L"u3" });
			defines.push_back({ L"PROBE_DATA_SPACE", L"space0" });

			return defines;
		};

		// Compile ProbeBlendingCS for Irradiance
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"1" });  // Blending irradiance
			defines.push_back({ L"OUTPUT_REGISTER", L"u1" });
			defines.push_back({ L"OUTPUT_SPACE", L"space0" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space0" });

			// Probe texel counts (irradiance uses 6x6 interior + 1 border = 8x8)
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_TEXELS", L"8" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_RAYS_PER_PROBE", L"256" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeBlendingCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeBlendingCS");

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeBlendingCS (Irradiance)");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_blendIrradiancePSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create irradiance blending PSO");
				return false;
			}
			m_blendIrradiancePSO->SetName(L"DDGI_BlendIrradiancePSO");
		}

		// Compile ProbeBlendingCS for Distance
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"0" });  // Blending distance
			defines.push_back({ L"OUTPUT_REGISTER", L"u2" });  // Distance uses u2
			defines.push_back({ L"OUTPUT_SPACE", L"space0" });

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

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeRelocationCS");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_relocationPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create relocation PSO");
				return false;
			}
			m_relocationPSO->SetName(L"DDGI_RelocationPSO");
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

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeRelocationCS (Reset)");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_relocationResetPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create relocation reset PSO");
				return false;
			}
			m_relocationResetPSO->SetName(L"DDGI_RelocationResetPSO");
		}

		// Compile ProbeClassificationCS (Update)
		{
			eastl::vector<DxcDefine> defines = createCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION", L"1" });

			std::wstring shaderPath = std::wstring(shaderBasePath) + L"ProbeClassificationCS.hlsl";
			winrt::com_ptr<IDxcBlob> shaderBlob;
			ShaderUtils::CompileShader(shaderBlob, shaderPath.c_str(), defines, L"cs_6_5", L"DDGIProbeClassificationCS");

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeClassificationCS");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_classificationPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create classification PSO");
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

			if (!shaderBlob) {
				logger::error("[DDGI] Failed to compile ProbeClassificationCS (Reset)");
				return false;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = m_rootSignature.get();
			psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

			HRESULT hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_classificationResetPSO.put()));
			if (FAILED(hr)) {
				logger::error("[DDGI] Failed to create classification reset PSO");
				return false;
			}
			m_classificationResetPSO->SetName(L"DDGI_ClassificationResetPSO");
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
