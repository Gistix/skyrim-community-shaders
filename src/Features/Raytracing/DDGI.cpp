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
			return false;

		m_device = device;
		m_descriptorHeap = descriptorHeap;
		m_descriptorHeapStartIndex = descriptorHeapStartIndex;
		m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		m_volumeDesc.name = const_cast<char*>("DDGIVolume_Main");
		m_volumeDesc.index = 0;
		m_volumeDesc.probeCounts = { m_settings.probeCounts[0], m_settings.probeCounts[1], m_settings.probeCounts[2] };
		m_volumeDesc.probeSpacing = { m_settings.probeSpacing[0], m_settings.probeSpacing[1], m_settings.probeSpacing[2] };
		m_volumeDesc.probeNumRays = m_settings.raysPerProbe;
		m_volumeDesc.probeMaxRayDistance = m_settings.maxRayDistance;
		m_volumeDesc.probeNumIrradianceInteriorTexels = 6;
		m_volumeDesc.probeNumDistanceInteriorTexels = 14;
		m_volumeDesc.probeNumIrradianceTexels = 8;
		m_volumeDesc.probeNumDistanceTexels = 16;
		m_volumeDesc.probeHysteresis = m_settings.hysteresis;
		m_volumeDesc.probeIrradianceThreshold = m_settings.irradianceThreshold;
		m_volumeDesc.probeBrightnessThreshold = m_settings.brightnessThreshold;
		m_volumeDesc.probeDistanceExponent = 50.0f;
		m_volumeDesc.probeIrradianceEncodingGamma = 5.0f;
		m_volumeDesc.probeViewBias = m_settings.viewBias;
		m_volumeDesc.probeNormalBias = m_settings.normalBias;
		m_volumeDesc.probeRelocationEnabled = m_settings.probeRelocationEnabled;
		m_volumeDesc.probeClassificationEnabled = m_settings.probeClassificationEnabled;
		m_volumeDesc.probeVariabilityEnabled = false;
		m_volumeDesc.probeMinFrontfaceDistance = m_settings.probeMinFrontfaceDistance;
		m_volumeDesc.probeRandomRayBackfaceThreshold = m_settings.probeRandomRayBackfaceThreshold;
		m_volumeDesc.probeFixedRayBackfaceThreshold = m_settings.probeFixedRayBackfaceThreshold;
		m_volumeDesc.movementType = m_settings.scrollingEnabled
			? rtxgi::EDDGIVolumeMovementType::Scrolling
			: rtxgi::EDDGIVolumeMovementType::Default;
		m_volumeDesc.probeRayDataFormat = rtxgi::EDDGIVolumeTextureFormat::F32x4;
		m_volumeDesc.probeIrradianceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
		m_volumeDesc.probeDistanceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x2;
		m_volumeDesc.probeDataFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
		m_volumeDesc.probeVariabilityFormat = rtxgi::EDDGIVolumeTextureFormat::F16;
		m_volumeDesc.showProbes = m_settings.showProbes;

		if (!CreateProbeTextures() || !CreateConstantsBuffer() || !CreateDDGIDescriptorHeap() ||
			!CreateRTVDescriptorHeap() || !CreateRootSignature() || !CreatePipelineStates())
		{
			logger::error("[DDGI] Failed to create resources");
			return false;
		}

		m_volume = eastl::make_unique<rtxgi::d3d12::DDGIVolume>();

		rtxgi::d3d12::DDGIVolumeResources resources = {};
		resources.unmanaged.enabled = true;
		resources.unmanaged.rootSignature = m_rootSignature.get();
		resources.unmanaged.probeRayData = m_probeRayData.get();
		resources.unmanaged.probeIrradiance = m_probeIrradiance.get();
		resources.unmanaged.probeDistance = m_probeDistance.get();
		resources.unmanaged.probeData = m_probeData.get();
		resources.unmanaged.probeVariability = m_probeVariability.get();
		resources.unmanaged.probeVariabilityAverage = m_probeVariabilityAverage.get();
		resources.unmanaged.probeVariabilityReadback = m_probeVariabilityReadback.get();
		resources.unmanaged.probeIrradianceRTV = m_probeIrradianceRTV;
		resources.unmanaged.probeDistanceRTV = m_probeDistanceRTV;
		resources.unmanaged.probeBlendingIrradiancePSO = m_blendIrradiancePSO.get();
		resources.unmanaged.probeBlendingDistancePSO = m_blendDistancePSO.get();
		resources.unmanaged.probeRelocation.updatePSO = m_relocationPSO.get();
		resources.unmanaged.probeRelocation.resetPSO = m_relocationResetPSO.get();
		resources.unmanaged.probeClassification.updatePSO = m_classificationPSO.get();
		resources.unmanaged.probeClassification.resetPSO = m_classificationResetPSO.get();
		resources.unmanaged.probeVariabilityPSOs.reductionPSO = m_variabilityReductionPSO.get();
		resources.unmanaged.probeVariabilityPSOs.extraReductionPSO = m_variabilityExtraReductionPSO.get();
		resources.unmanaged.rootParamSlotRootConstants = 0;
		resources.unmanaged.rootParamSlotResourceDescriptorTable = 1;
		resources.constantsBuffer = m_constantsBuffer.get();
		resources.constantsBufferUpload = m_constantsBufferUpload.get();
		resources.constantsBufferSizeInBytes = m_constantsBufferSize;
		resources.descriptorHeap.resources = m_ddgiDescriptorHeap.get();
		resources.descriptorHeap.entrySize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		resources.descriptorHeap.constantsIndex = 0;
		resources.descriptorHeap.resourceIndices.rayDataUAVIndex = 1;
		resources.descriptorHeap.resourceIndices.probeIrradianceUAVIndex = 2;
		resources.descriptorHeap.resourceIndices.probeDistanceUAVIndex = 3;
		resources.descriptorHeap.resourceIndices.probeDataUAVIndex = 4;
		resources.descriptorHeap.resourceIndices.probeVariabilityUAVIndex = 5;
		resources.descriptorHeap.resourceIndices.probeVariabilityAverageUAVIndex = 6;

		rtxgi::ERTXGIStatus status = m_volume->Create(m_volumeDesc, resources);
		if (status != rtxgi::ERTXGIStatus::OK)
		{
			logger::error("[DDGI] Failed to create volume, status: {}", static_cast<int>(status));
			return false;
		}

		m_initialized = true;
		logger::info("[DDGI] Initialized {}x{}x{} probes", m_volumeDesc.probeCounts.x, m_volumeDesc.probeCounts.y, m_volumeDesc.probeCounts.z);
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

	void DDGIManager::Reinitialize()
	{
		if (!m_device)
			return;

		ID3D12Device5* device = m_device;
		ID3D12DescriptorHeap* heap = m_descriptorHeap;
		UINT heapStartIndex = m_descriptorHeapStartIndex;

		Shutdown();
		m_firstFrame = true;
		Initialize(device, heap, heapStartIndex);
	}

	void DDGIManager::Update(const DirectX::XMFLOAT3& cameraPosition, const DirectX::XMFLOAT3& cameraForward)
	{
		if (!IsEnabled())
			return;

		rtxgi::float3 anchor = {
			cameraPosition.x + cameraForward.x * m_settings.scrollForwardBias,
			cameraPosition.y + cameraForward.y * m_settings.scrollForwardBias,
			cameraPosition.z + cameraForward.z * m_settings.scrollForwardBias
		};

		if (m_firstFrame)
		{
			m_volume->SetOrigin(anchor);
			m_firstFrame = false;
		}

		if (m_settings.scrollingEnabled)
			m_volume->SetScrollAnchor(anchor);
		else
			m_volume->SetOrigin(anchor);

		m_volume->Update();

		rtxgi::float3 origin = m_volume->GetOrigin();
		m_volumeOrigin = { origin.x, origin.y, origin.z };
		m_lastCameraPosition = cameraPosition;
		m_frameCounter++;
	}

	void DDGIManager::UpdateProbes(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		rtxgi::d3d12::DDGIVolume* volumes[] = { m_volume.get() };
		rtxgi::d3d12::UpdateDDGIVolumeProbes(cmdList, 1, volumes);

		if (m_settings.probeRelocationEnabled && (m_frameCounter % RELOCATION_INTERVAL == 0))
			rtxgi::d3d12::RelocateDDGIVolumeProbes(cmdList, 1, volumes);

		if (m_settings.probeClassificationEnabled && (m_frameCounter % RELOCATION_INTERVAL == 0))
			rtxgi::d3d12::ClassifyDDGIVolumeProbes(cmdList, 1, volumes);
	}

	void DDGIManager::UploadConstants(ID3D12GraphicsCommandList* cmdList)
	{
		if (!IsEnabled() || !m_volume)
			return;

		rtxgi::DDGIVolumeDescGPUPacked packedDesc = m_volume->GetDescGPUPacked();

		void* pData = nullptr;
		CD3DX12_RANGE readRange(0, 0);
		m_constantsBufferUpload->Map(0, &readRange, &pData);
		memcpy(pData, &packedDesc, sizeof(packedDesc));
		m_constantsBufferUpload->Unmap(0, nullptr);

		D3D12_RESOURCE_STATES prevState = m_constantsUploaded
			? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
			: D3D12_RESOURCE_STATE_COMMON;

		CD3DX12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
			m_constantsBuffer.get(), prevState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmdList->ResourceBarrier(1, &toCopy);

		cmdList->CopyBufferRegion(m_constantsBuffer.get(), 0, m_constantsBufferUpload.get(), 0, sizeof(packedDesc));

		CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
			m_constantsBuffer.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		cmdList->ResourceBarrier(1, &toSRV);

		m_constantsUploaded = true;
	}

	void DDGIManager::BeginFrame(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		if (!m_probeTexturesInSRVState) {
			CD3DX12_RESOURCE_BARRIER barriers[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeIrradiance.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeDistance.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeData.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			};
			cmdList->ResourceBarrier(_countof(barriers), barriers);
			m_probeTexturesInSRVState = true;
		}
	}

	void DDGIManager::BeginProbeTracing(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		UploadConstants(cmdList);

		CD3DX12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_probeRayData.get());
		cmdList->ResourceBarrier(1, &uavBarrier);
	}

	void DDGIManager::BeginProbeBlending(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		if (m_probeTexturesInSRVState) {
			CD3DX12_RESOURCE_BARRIER barriers[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeIrradiance.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeDistance.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeData.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			};
			cmdList->ResourceBarrier(_countof(barriers), barriers);
			m_probeTexturesInSRVState = false;
		}

		CD3DX12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_probeRayData.get());
		cmdList->ResourceBarrier(1, &uavBarrier);

		UploadConstants(cmdList);
	}

	void DDGIManager::BeginScreenSampling(ID3D12GraphicsCommandList4* cmdList)
	{
		if (!IsEnabled())
			return;

		if (!m_probeTexturesInSRVState) {
			CD3DX12_RESOURCE_BARRIER barriers[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeIrradiance.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeDistance.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_probeData.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			};
			cmdList->ResourceBarrier(_countof(barriers), barriers);
			m_probeTexturesInSRVState = true;
		}
	}

	void DDGIManager::EndFrame([[maybe_unused]] ID3D12GraphicsCommandList4* cmdList)
	{
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
		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::RayData,
			m_volumeDesc.probeRayDataFormat, m_probeRayData, L"DDGI_ProbeRayData"))
			return false;

		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::Irradiance,
			m_volumeDesc.probeIrradianceFormat, m_probeIrradiance, L"DDGI_ProbeIrradiance", true))
			return false;

		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::Distance,
			m_volumeDesc.probeDistanceFormat, m_probeDistance, L"DDGI_ProbeDistance", true))
			return false;

		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::Data,
			m_volumeDesc.probeDataFormat, m_probeData, L"DDGI_ProbeData"))
			return false;

		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::Variability,
			m_volumeDesc.probeVariabilityFormat, m_probeVariability, L"DDGI_ProbeVariability"))
			return false;

		if (!CreateDDGITexture(rtxgi::EDDGIVolumeTextureType::VariabilityAverage,
			rtxgi::EDDGIVolumeTextureFormat::F32x2, m_probeVariabilityAverage, L"DDGI_ProbeVariabilityAverage"))
			return false;

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

		// Create SRVs in main GI descriptor heap (Irradiance, Distance, Data at indices +1, +2, +3)
		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
		cpuHandle.ptr += (m_descriptorHeapStartIndex + 1) * m_descriptorSize;

		auto createTexture2DArraySRV = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2DArray.MostDetailedMip = 0;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.FirstArraySlice = 0;
			srvDesc.Texture2DArray.ArraySize = resource->GetDesc().DepthOrArraySize;
			m_device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
			cpuHandle.ptr += m_descriptorSize;
		};

		createTexture2DArraySRV(m_probeIrradiance.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat));
		createTexture2DArraySRV(m_probeDistance.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat));
		createTexture2DArraySRV(m_probeData.get(),
			rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Data, m_volumeDesc.probeDataFormat));

		return true;
	}

	bool DDGIManager::CreateConstantsBuffer()
	{
		m_constantsBufferSize = (sizeof(rtxgi::DDGIVolumeDescGPUPacked) + 255) & ~255ULL;

		CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(m_constantsBufferSize);
		CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
		CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

		if (FAILED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_constantsBuffer.put()))))
			return false;
		m_constantsBuffer->SetName(L"DDGI_ConstantsBuffer");

		if (FAILED(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_constantsBufferUpload.put()))))
			return false;
		m_constantsBufferUpload->SetName(L"DDGI_ConstantsBufferUpload");

		return true;
	}

	bool DDGIManager::CreateRTVDescriptorHeap()
	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = 2;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_rtvDescriptorHeap.put()))))
			return false;
		m_rtvDescriptorHeap->SetName(L"DDGI_RTVDescriptorHeap");

		UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

		auto createRTV = [&](ID3D12Resource* resource, rtxgi::EDDGIVolumeTextureType type, rtxgi::EDDGIVolumeTextureFormat format) {
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, format);
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtvDesc.Texture2DArray.MipSlice = 0;
			rtvDesc.Texture2DArray.FirstArraySlice = 0;
			rtvDesc.Texture2DArray.ArraySize = 1;
			rtvDesc.Texture2DArray.PlaneSlice = 0;
			m_device->CreateRenderTargetView(resource, &rtvDesc, rtvHandle);
			D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHandle;
			rtvHandle.ptr += rtvDescriptorSize;
			return handle;
		};

		m_probeIrradianceRTV = createRTV(m_probeIrradiance.get(), rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat);
		m_probeDistanceRTV = createRTV(m_probeDistance.get(), rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat);

		return true;
	}

	bool DDGIManager::CreateDDGIDescriptorHeap()
	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = DDGI_DESCRIPTOR_COUNT;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_ddgiDescriptorHeap.put()))))
			return false;
		m_ddgiDescriptorHeap->SetName(L"DDGI_ResourceDescriptorHeap");

		UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_ddgiDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

		// [0] Constants SRV
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

		auto createUAV = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
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

		createUAV(m_probeRayData.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::RayData, m_volumeDesc.probeRayDataFormat));
		createUAV(m_probeIrradiance.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Irradiance, m_volumeDesc.probeIrradianceFormat));
		createUAV(m_probeDistance.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Distance, m_volumeDesc.probeDistanceFormat));
		createUAV(m_probeData.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Data, m_volumeDesc.probeDataFormat));
		createUAV(m_probeVariability.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::Variability, m_volumeDesc.probeVariabilityFormat));
		createUAV(m_probeVariabilityAverage.get(), rtxgi::d3d12::GetDDGIVolumeTextureFormat(rtxgi::EDDGIVolumeTextureType::VariabilityAverage, rtxgi::EDDGIVolumeTextureFormat::F32x2));

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
		if (width == 0 || height == 0 || arraySize == 0)
			return false;

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

		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			initialState, nullptr, IID_PPV_ARGS(outResource.put()))))
			return false;

		outResource->SetName(debugName);
		return true;
	}

	bool DDGIManager::CreateDDGITexture(rtxgi::EDDGIVolumeTextureType type,
		rtxgi::EDDGIVolumeTextureFormat formatEnum,
		winrt::com_ptr<ID3D12Resource>& out,
		const wchar_t* name, bool allowRT)
	{
		uint32_t width, height, arraySize;
		rtxgi::GetDDGIVolumeTextureDimensions(m_volumeDesc, type, width, height, arraySize);
		DXGI_FORMAT format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, formatEnum);
		return CreateTexture2DArray(width, height, arraySize, format,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out, name, allowRT);
	}

	bool DDGIManager::CreateRootSignature()
	{
		rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc heapDesc = {};
		heapDesc.constantsIndex = 0;
		heapDesc.resourceIndices.rayDataUAVIndex = 1;
		heapDesc.resourceIndices.probeIrradianceUAVIndex = 2;
		heapDesc.resourceIndices.probeDistanceUAVIndex = 3;
		heapDesc.resourceIndices.probeDataUAVIndex = 4;
		heapDesc.resourceIndices.probeVariabilityUAVIndex = 5;
		heapDesc.resourceIndices.probeVariabilityAverageUAVIndex = 6;

		ID3DBlob* signatureBlob = nullptr;
		if (!rtxgi::d3d12::GetDDGIVolumeRootSignatureDesc(heapDesc, signatureBlob))
			return false;

		HRESULT hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
			signatureBlob->GetBufferSize(), IID_PPV_ARGS(m_rootSignature.put()));
		signatureBlob->Release();

		return SUCCEEDED(hr);
	}

	eastl::vector<DxcDefine> DDGIManager::GetCommonDefines() const
	{
		eastl::vector<DxcDefine> defines;
		defines.push_back({ L"HLSL", L"1" });
		defines.push_back({ L"RTXGI_DDGI_RESOURCE_MANAGEMENT", L"0" });
		defines.push_back({ L"RTXGI_DDGI_SHADER_REFLECTION", L"0" });
		defines.push_back({ L"RTXGI_DDGI_BINDLESS_RESOURCES", L"0" });
		defines.push_back({ L"RTXGI_DDGI_BLEND_SHARED_MEMORY", L"1" });
		defines.push_back({ L"RTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY", L"1" });
		defines.push_back({ L"RTXGI_COORDINATE_SYSTEM", L"0" });
		defines.push_back({ L"VOLUME_CONSTS_REGISTER", L"t0" });
		defines.push_back({ L"VOLUME_CONSTS_SPACE", L"space1" });
		defines.push_back({ L"RAY_DATA_REGISTER", L"u0" });
		defines.push_back({ L"RAY_DATA_SPACE", L"space1" });
		defines.push_back({ L"PROBE_DATA_REGISTER", L"u3" });
		defines.push_back({ L"PROBE_DATA_SPACE", L"space1" });
		defines.push_back({ L"CONSTS_REGISTER", L"b0" });
		defines.push_back({ L"CONSTS_SPACE", L"space1" });
		return defines;
	}

	bool DDGIManager::CompileAndCreatePSO(const wchar_t* shaderPath,
		const eastl::vector<DxcDefine>& defines,
		const wchar_t* entryPoint,
		winrt::com_ptr<ID3D12PipelineState>& outPSO,
		const wchar_t* psoName)
	{
		winrt::com_ptr<IDxcBlob> shaderBlob;
		ShaderUtils::CompileShader(shaderBlob, shaderPath, defines, L"cs_6_5", entryPoint);
		if (!shaderBlob || shaderBlob->GetBufferSize() == 0)
			return false;

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_rootSignature.get();
		psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };

		if (FAILED(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(outPSO.put()))))
			return false;

		outPSO->SetName(psoName);
		return true;
	}

	bool DDGIManager::CreatePipelineStates()
	{
		const wchar_t* blendPath = L"Data/Shaders/Raytracing/DDGI/ProbeBlendingCS.hlsl";
		const wchar_t* relocationPath = L"Data/Shaders/Raytracing/DDGI/ProbeRelocationCS.hlsl";
		const wchar_t* classificationPath = L"Data/Shaders/Raytracing/DDGI/ProbeClassificationCS.hlsl";
		const wchar_t* reductionPath = L"Data/Shaders/Raytracing/DDGI/ReductionCS.hlsl";

		// Irradiance blending
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"1" });
			defines.push_back({ L"OUTPUT_REGISTER", L"u1" });
			defines.push_back({ L"OUTPUT_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_TEXELS", L"8" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_RAYS_PER_PROBE", L"256" });
			if (!CompileAndCreatePSO(blendPath, defines, L"DDGIProbeBlendingCS", m_blendIrradiancePSO, L"DDGI_BlendIrradiancePSO"))
				return false;
		}

		// Distance blending
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_BLEND_RADIANCE", L"0" });
			defines.push_back({ L"OUTPUT_REGISTER", L"u2" });
			defines.push_back({ L"OUTPUT_SPACE", L"space1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_TEXELS", L"16" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"14" });
			defines.push_back({ L"RTXGI_DDGI_BLEND_RAYS_PER_PROBE", L"256" });
			if (!CompileAndCreatePSO(blendPath, defines, L"DDGIProbeBlendingCS", m_blendDistancePSO, L"DDGI_BlendDistancePSO"))
				return false;
		}

		// Relocation (Update)
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_STATE_CLASSIFIER", L"0" });
			if (!CompileAndCreatePSO(relocationPath, defines, L"DDGIProbeRelocationCS", m_relocationPSO, L"DDGI_RelocationPSO"))
				return false;
		}

		// Relocation (Reset)
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_RELOCATION_RESET", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_STATE_CLASSIFIER", L"0" });
			if (!CompileAndCreatePSO(relocationPath, defines, L"DDGIProbeRelocationCS", m_relocationResetPSO, L"DDGI_RelocationResetPSO"))
				return false;
		}

		// Classification (Update)
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION", L"1" });
			if (!CompileAndCreatePSO(classificationPath, defines, L"DDGIProbeClassificationCS", m_classificationPSO, L"DDGI_ClassificationPSO"))
				return false;
		}

		// Classification (Reset)
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION", L"1" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_CLASSIFICATION_RESET", L"1" });
			if (!CompileAndCreatePSO(classificationPath, defines, L"DDGIProbeClassificationCS", m_classificationResetPSO, L"DDGI_ClassificationResetPSO"))
				return false;
		}

		// Variability Reduction
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_WAVE_LANE_COUNT", L"32" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_REGISTER", L"u5" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_SPACE", L"space1" });
			if (!CompileAndCreatePSO(reductionPath, defines, L"DDGIReductionCS", m_variabilityReductionPSO, L"DDGI_VariabilityReductionPSO"))
				return false;
		}

		// Variability Extra Reduction
		{
			auto defines = GetCommonDefines();
			defines.push_back({ L"RTXGI_DDGI_WAVE_LANE_COUNT", L"32" });
			defines.push_back({ L"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", L"6" });
			defines.push_back({ L"PROBE_VARIABILITY_REGISTER", L"u4" });
			defines.push_back({ L"PROBE_VARIABILITY_SPACE", L"space1" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_REGISTER", L"u5" });
			defines.push_back({ L"PROBE_VARIABILITY_AVERAGE_SPACE", L"space1" });
			if (!CompileAndCreatePSO(reductionPath, defines, L"DDGIExtraReductionCS", m_variabilityExtraReductionPSO, L"DDGI_VariabilityExtraReductionPSO"))
				return false;
		}

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
