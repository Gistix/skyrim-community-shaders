/*
* This file accompanies VoxelConeTracingGI.h
* Please refer to the header for more information.
*
* ProfJack
* 2025-06-28
*/

#include "VoxelConeTracingGI.h"
#include "LightLimitFix.h"
#include "InverseSquareLighting.h"

#include "Globals.h"
#include "State.h"
#include "Deferred.h"
#include "Utils/UI.h"

static constexpr uint MAX_LIGHTS = 1024;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VoxelConeTracingGI::Settings,
	noIndoorDir,
	lod0Resolution,
	lod0Size
)

////////////////////////////////////////////////////////////////////////////////////

void VoxelConeTracingGI::RestoreDefaultSettings()
{
	settings = {};
}

void VoxelConeTracingGI::LoadSettings(json& o_json)
{
	settings = o_json;
}

void VoxelConeTracingGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VoxelConeTracingGI::DrawSettings()
{
	ImGui::SeparatorText("Tweaks");
	ImGui::Checkbox("Disable Indoor Directional", (bool*)&settings.noIndoorDir);

	ImGui::SeparatorText("Settings");
	ImGui::Combo("Lod0 Resolution", &settings.lod0Resolution, dimensionsLabels, IM_ARRAYSIZE(dimensionsLabels));
	ImGui::InputInt("Lod0 Size", &settings.lod0Size);
	
	ImGui::SeparatorText("Debug");
	ImGui::LabelText("Vertex buffers", "%zu", vertexBuffers.size());
	ImGui::LabelText("Index buffers", "%zu", indexBuffers.size());
	ImGui::LabelText("Input layouts", "%zu", inputLayouts.size());

	BUFFER_VIEWER_NODE_BULLET(debugTex, 0.5f);
}

void VoxelConeTracingGI::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating Constant buffers...");
	{
		voxelizeCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VoxelizeCB>());	
		injectLightingCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<InjectLightingCB>());	
		debugCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DebugCB>());	
	}

	logger::debug("Creating Structured buffers...");
	{
		uint lod0MaxDepth = OctreeMaxDepth(0);
		uint lod0NodeCount = NodeCount(lod0MaxDepth);
		uint lod0LeafNodeCount = LeafNodeCount(lod0MaxDepth);

		// Node buffer
		eastl::vector<Node> nodes(lod0NodeCount);
		nodes.at(0) = Node{ { 0, 0, 0 }, 0, 0, { 0 } };

		auto nodeBufferDesc = StructuredBufferDesc<Node>(lod0NodeCount, true, false);
		nodeBuffer = eastl::make_unique<StructuredBuffer>(nodeBufferDesc, nodes.data(), lod0NodeCount);
		nodeBuffer->CreateUAV(); // TODO: use D3D11_BUFFER_UAV_FLAG_APPEND :pray:
		nodeBuffer->CreateSRV();

		// Voxel Buffer
		eastl::vector<Voxel> voxels(lod0LeafNodeCount);
		auto voxelBufferDesc = StructuredBufferDesc<Voxel>(lod0LeafNodeCount, true, false);
		voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, voxels.data(), lod0LeafNodeCount);
		voxelBuffer->CreateUAV();
		voxelBuffer->CreateSRV();

		// Light buffer
		auto lightBufferDesc = StructuredBufferDesc<Light>(MAX_LIGHTS, false, true);
		lightBuffer = eastl::make_unique<StructuredBuffer>(lightBufferDesc, MAX_LIGHTS);
		lightBuffer->CreateSRV();
	}

	logger::debug("Creating Raw buffers...");
	{
		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = 4,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
		};

		uint initialNodeCount = 1;
		D3D11_SUBRESOURCE_DATA initialData = {
			.pSysMem = &initialNodeCount
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = { 
				.FirstElement = 0,
				.NumElements = 1,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW
			}
		};
		
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,  // DXGI_FORMAT_R32_UINT
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX,
			.BufferEx = {
				.FirstElement = 0,
				.NumElements = 1,
				.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW
			}
		};

		// Node count buffer
		nodeCountBuffer = eastl::make_unique<Buffer>(bufferDesc, &initialData);
		nodeCountBuffer->CreateUAV(uavDesc);
		nodeCountBuffer->CreateSRV(srvDesc);

		// Voxel count buffer
		voxelCountBuffer = eastl::make_unique<Buffer>(bufferDesc);
		voxelCountBuffer->CreateUAV(uavDesc);
		voxelCountBuffer->CreateSRV(srvDesc);

		D3D11_BUFFER_DESC argsBufferDesc = {
			.ByteWidth = 12,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC argsUAVDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = 3,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW 
			}
		};

		// Voxel Args buffer
		voxelArgsBuffer = eastl::make_unique<Buffer>(argsBufferDesc);
		voxelArgsBuffer->CreateUAV(argsUAVDesc);
	}

	logger::debug("Creating textures...");
	{
		// Volume texture
		{
			auto lod0Resolution = GetLodResolution(0);
			uint lod0MipLevels = MipLevels(lod0Resolution);

			D3D11_TEXTURE3D_DESC lod0TexDesc{
				.Width = lod0Resolution,
				.Height = lod0Resolution,
				.Depth = lod0Resolution,
				.MipLevels = lod0MipLevels,
				.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				.CPUAccessFlags = 0,
				.MiscFlags = 0
			};

			D3D11_SHADER_RESOURCE_VIEW_DESC lod0SRVDesc = {
				.Format = lod0TexDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
				.Texture3D = {
					.MostDetailedMip = 0,
					.MipLevels = lod0TexDesc.MipLevels 
				}
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC lod0UAVDesc = {
				.Format = lod0TexDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
				.Texture3D = {
					.MipSlice = 0,
					.FirstWSlice = 0,
					.WSize = lod0TexDesc.Depth 
				}
			};

			lod0Tex = eastl::make_unique<Texture3D>(lod0TexDesc);
			lod0Tex->CreateSRV(lod0SRVDesc);
			lod0Tex->CreateUAV(lod0UAVDesc);
		}

		// Debug texture
		{
			auto renderer = globals::game::renderer;
			auto screenSize = renderer->GetScreenSize();

			D3D11_TEXTURE2D_DESC texDesc = {
				.Width = screenSize.width,
				.Height = screenSize.height,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
				.SampleDesc = { 
					.Count = 1, 
					.Quality = 0 
				},
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				.CPUAccessFlags = 0,
				.MiscFlags = 0
			};

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = 1
				}
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MipSlice = 0 
				}
			};

			debugTex = eastl::make_unique<Texture2D>(texDesc); 
			debugTex->CreateSRV(srvDesc);
			debugTex->CreateUAV(uavDesc);
		}

		logger::debug("Creating samplers...");
		{
			D3D11_SAMPLER_DESC lod0SamplerDesc = {
				.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
				.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
				.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
				.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
				.MinLOD = 0,
				.MaxLOD = D3D11_FLOAT32_MAX
			};
			DX::ThrowIfFailed(device->CreateSamplerState(&lod0SamplerDesc, lod0Sampler.put()));
		}

	}

	// Depth copy texture
	{
		auto renderer = globals::game::renderer;
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		auto depthTexture = depth.texture;

		D3D11_TEXTURE2D_DESC depthDesc;
		depthTexture->GetDesc(&depthDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc;
		depth.depthSRV->GetDesc(&depthSRVDesc);

		prevDepth = eastl::make_unique<Texture2D>(depthDesc);
		prevDepth->CreateSRV(depthSRVDesc);
	}

	// Raster State
	{
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.DepthClipEnable = TRUE;
		rasterDesc.MultisampleEnable = TRUE;

		device->CreateRasterizerState(&rasterDesc, voxelRasterState.put());
	}

	CompileShaders();
}

#pragma region VoxelizationBuffer

void VoxelConeTracingGI::InstallD3DHooks()
{
	logger::info("[VCT] D3D hooks installed");

	stl::detour_vfunc<3, VoxelConeTracingGI::Hooks::ID3D11Device_CreateBuffer>(globals::d3d::device);
	stl::detour_vfunc<11, VoxelConeTracingGI::Hooks::ID3D11Device_CreateInputLayout>(globals::d3d::device);
}

/*VoxelConeTracingGI::BufferData VoxelConeTracingGI::AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData)
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	BufferData data;
	data.width = pDesc->ByteWidth;

	{
		D3D11_MAPPED_SUBRESOURCE mappedResource;
		DX::ThrowIfFailed(context->Map(data.buffer.get(), 0, D3D11_MAP_READ, 0, &mappedResource));
		memcpy(mappedResource.pData, pInitialData->pSysMem, pDesc->ByteWidth);
		context->Unmap(data.buffer.get(), 0);
	}

	return data;
}*/

/*VoxelConeTracingGI::BufferData VoxelConeTracingGI::AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData)
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	BufferData data;

	// Allocate the memory on the CPU and GPU
	{
		ID3D11Buffer* buffer;
		DX::ThrowIfFailed(device->CreateBuffer(pDesc, nullptr, &buffer));
		data.buffer.attach(buffer);
		data.width = pDesc->ByteWidth;
	}

	// Map the buffer to CPU memory and copy the vertex data
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(data.buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, pInitialData->pSysMem, pDesc->ByteWidth);
		context->Unmap(data.buffer.get(), 0);
	}

	return data;
}*/

VoxelConeTracingGI::BufferData VoxelConeTracingGI::AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData)
{
	auto device = globals::d3d::device;

	BufferData data;
	{
		ID3D11Buffer* buffer = nullptr;
		DX::ThrowIfFailed(Hooks::ID3D11Device_CreateBuffer::func(device, pDesc, pInitialData, &buffer));
		data.buffer.attach(buffer);
		data.width = pDesc->ByteWidth;
	}

	return data;
}

struct ID3D11Buffer_Release
{
	static void thunk(ID3D11Buffer* This)
	{
		VoxelConeTracingGI::GetSingleton()->UnregisterVertexBuffer(This);
		VoxelConeTracingGI::GetSingleton()->UnregisterIndexBuffer(This);
		func(This);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

bool hooked = false;

void VoxelConeTracingGI::RegisterVertexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
	std::lock_guard lock{ mutex };

	BufferData data = AllocateBuffer(pDesc, pInitialData);
	vertexBuffers.insert({ *ppBuffer, data });
	if (!hooked) {
		stl::detour_vfunc<2, ID3D11Buffer_Release>(*ppBuffer);
		hooked = true;
	}
}

void VoxelConeTracingGI::RegisterIndexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
	std::lock_guard lock{ mutex };

	BufferData data = AllocateBuffer(pDesc, pInitialData);
	indexBuffers.insert({ *ppBuffer, data });
	if (!hooked) {
		stl::detour_vfunc<2, ID3D11Buffer_Release>(*ppBuffer);
		hooked = true;
	}
}

size_t BytesPerElement(DXGI_FORMAT fmt)
{
	// This list only includes those formats that are valid for use by IB or VB

	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 16;

	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		return 12;

	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		return 8;

	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return 4;

	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
		return 2;

	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
		return 1;

	case DXGI_FORMAT_B4G4R4A4_UNORM:
		return 2;

	default:
		// No BC, sRGB, XRBias, SharedExp, Typeless, Depth, or Video formats
		return 0;
	}
}

void VoxelConeTracingGI::RegisterInputLayout(ID3D11InputLayout* ppInputLayout, D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements)
{
	InputLayoutData data = {};
	for (UINT i = 0; i < NumElements; i++) {
		if (strcmp(pInputElementDescs[i].SemanticName, "POSITION") == 0) {
			data.stride = 0;

			for (UINT k = 0; k < NumElements; k++) {
				data.stride += (UINT)BytesPerElement(pInputElementDescs[k].Format);
			}

			data.offset = 0;

			for (UINT k = 0; k < i; k++) {
				data.offset += (UINT)BytesPerElement(pInputElementDescs[k].Format);
			}

			auto format = pInputElementDescs[i].Format;
			if (format == DXGI_FORMAT_R32G32B32A32_FLOAT || format == DXGI_FORMAT_R32G32B32_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
				data.format = format;
			} else {
				return;
			}

			inputLayouts.insert({ ppInputLayout, data });
		}
	}
}

void VoxelConeTracingGI::UnregisterVertexBuffer(ID3D11Buffer* ppBuffer)
{
	//std::lock_guard lock{ mutex };

	vertexBuffers.erase(ppBuffer);
}

void VoxelConeTracingGI::UnregisterIndexBuffer(ID3D11Buffer* ppBuffer)
{
	//std::lock_guard lock{ mutex };

	indexBuffers.erase(ppBuffer);
}

void VoxelConeTracingGI::FrameUpdate()
{
	logger::info("Vertex buffers - {}", vertexBuffers.size());
	logger::info("Index buffers - {}", indexBuffers.size());
	logger::info("Input layouts - {}", inputLayouts.size());
	logger::info("Vert desc to layouts - {}", vertexDescToInputLayout.size());
	
	auto state = globals::state;
	auto context = globals::d3d::context;

	state->BeginPerfEvent("VoxelConeTracing - FrameUpdate");
	//std::lock_guard lock{ mutex };
	//auto context = globals::d3d::context;

	/*
	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	gContext->IASetInputLayout(inputLayout);
	gContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
	gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	gContext->VSSetShader(vertexShader, nullptr, 0);
	gContext->PSSetShader(pixelShader, nullptr, 0);

	gContext->Draw(3, 0); // 3 vertices, start at index 0 
	*/

	for (auto const& keyVal: inputLayouts) {
		auto inputLayout = keyVal.first;
		auto layoutData = keyVal.second;

		context->IASetInputLayout(inputLayout);
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &layoutData.stride, &layoutData.offset);
		context->IASetPrimitiveTopology(layoutData.format); // D3D11_PRIMITIVE_TOPOLOGY

		context->VSSetShader(vertexShader, nullptr, 0);
		context->PSSetShader(pixelShader, nullptr, 0);

		context->DrawIndexedInstanced(3, 0);  // 3 vertices, start at index 0 
	}

	state->EndPerfEvent();
}

DirectX::XMMATRIX GetXMFromNiTransform(const RE::NiTransform& Transform)
{
	DirectX::XMMATRIX temp;

	const RE::NiMatrix3& m = Transform.rotate;
	const float scale = Transform.scale;

	temp.r[0] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][0],
										   m.entry[1][0],
										   m.entry[2][0],
										   0.0f),
		scale);

	temp.r[1] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][1],
										   m.entry[1][1],
										   m.entry[2][1],
										   0.0f),
		scale);

	temp.r[2] = DirectX::XMVectorScale(DirectX::XMVectorSet(
										   m.entry[0][2],
										   m.entry[1][2],
										   m.entry[2][2],
										   0.0f),
		scale);

	temp.r[3] = DirectX::XMVectorSet(
		Transform.translate.x,
		Transform.translate.y,
		Transform.translate.z,
		1.0f);

	return temp;
}

void VoxelConeTracingGI::AddInstance(RE::BSTriShape* a_geometry)
{
	const auto& transform = a_geometry->world;
	const auto& modelData = a_geometry->GetModelData().modelBound;
	if (a_geometry->worldBound.radius == 0)
		return;

	auto effect = a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect];
	auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect.get());

	if (!lightingShader)
		return;

	if (!lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite, RE::BSShaderProperty::EShaderPropertyFlag::kZBufferTest))
		return;

	if (lightingShader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODObjects, RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape, RE::BSShaderProperty::EShaderPropertyFlag::kHDLODObjects))
		return;

	if (lightingShader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned))
		return;

	// TODO: FIX THIS
	if (lightingShader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape, RE::BSShaderProperty::EShaderPropertyFlag::kMultipleTextures, RE::BSShaderProperty::EShaderPropertyFlag::kMultiIndexSnow))
		return;

	const RE::NiPoint3 radius = { modelData.radius, modelData.radius, modelData.radius };
	const RE::NiPoint3 aabbMinVec = transform * (modelData.center - radius);
	const RE::NiPoint3 aabbMaxVec = transform * (modelData.center + radius);

	auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;

	if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
		return;

	BufferData* vertexBuffer;
	BufferData* indexBuffer;

	{
		auto it2 = vertexBuffers.find((ID3D11Buffer*)rendererData->vertexBuffer);
		if (it2 == vertexBuffers.end())
			return;
		vertexBuffer = &it2->second;
	}

	{
		auto it2 = indexBuffers.find((ID3D11Buffer*)rendererData->indexBuffer);
		if (it2 == indexBuffers.end())
			return;
		indexBuffer = &it2->second;
	}

	auto vertexDesc = *(uint64_t*)&rendererData->vertexDesc;

	InputLayoutData* inputLayoutData;

	{
		auto it2 = vertexDescToInputLayout.find(vertexDesc);
		if (it2 == vertexDescToInputLayout.end())
			return;
		auto inputLayout = it2->second;
		auto it3 = inputLayouts.find(inputLayout);
		if (it3 == inputLayouts.end())
			return;
		inputLayoutData = &it3->second;
	}

	InstanceData instanceData = {};

	{
		instanceData.min = { aabbMinVec.x, aabbMinVec.y, aabbMinVec.z };
		instanceData.max = { aabbMaxVec.x, aabbMaxVec.y, aabbMaxVec.z };
	}

	float4x4 xmmTransform = GetXMFromNiTransform(transform);

	for (uint row = 0; row < 3; ++row) {
		for (uint col = 0; col < 4; ++col) {
			instanceData.transform[row * 4 + col] = xmmTransform.m[col][row];
		}
	}

	instances.insert({ a_geometry, instanceData });
}

#pragma endregion

void VoxelConeTracingGI::Voxelize()
{
	auto state = globals::state;
	state->BeginPerfEvent("Voxelize");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Voxelize");

	auto context = globals::d3d::context;

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;

	float2 res = globals::state->screenSize;
	float2 size = Util::ConvertToDynamic(res);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };

	std::array<ID3D11ShaderResourceView*, 3> srvs = { 
		rts[ALBEDO].SRV,
		prevDepth->srv.get(),
		rts[NORMALROUGHNESS].SRV,
	};

	std::array<ID3D11UnorderedAccessView*, 4> uavs = {
		nodeBuffer->UAV(),
		nodeCountBuffer->uav.get(),
		voxelBuffer->UAV(),
		voxelCountBuffer->uav.get()
	};

	float lod0Size = static_cast<float>(settings.lod0Size) / Util::Units::GAME_UNIT_TO_M;

	auto eye = Util::GetCameraData(0);

	float2 ndcToViewMult = float2(2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1));
	float2 ndcToViewAdd = float2(-1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1));

	auto lod0Resolution = GetLodResolution(0);
	float lod0ResolutionFloat = static_cast<float>(lod0Resolution);

	voxelizeCBData.NDCToView = float4(ndcToViewMult.x, ndcToViewMult.y, ndcToViewAdd.x, ndcToViewAdd.y);
	voxelizeCBData.RcpFrameDim = float2(1.0) / size;
	voxelizeCBData.Cell2Coord = 1.0f / lod0ResolutionFloat;
	voxelizeCBData.Resolution = lod0ResolutionFloat;
	voxelizeCBData.Center = center;
	voxelizeCBData.MaxDepth = OctreeMaxDepth(0);
	voxelizeCBData.Size = float3(lod0Size, lod0Size, lod0Size);

	voxelizeCB->Update(voxelizeCBData);

	auto cb = voxelizeCB->CB();

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	context->CSSetConstantBuffers(0, 1, &cb);

	context->CSSetShader(voxelizeCompute.get(), nullptr, 0);
	context->Dispatch(resolution[0] >> 3, resolution[1] >> 3, 1);

	srvs.fill(nullptr);
	uavs.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CopyResource(prevDepth->resource.get(), depth.texture);

	state->EndPerfEvent();
}

void VoxelConeTracingGI::VoxelArgs()
{
	auto state = globals::state;
	state->BeginPerfEvent("Voxel Args");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Voxel Args");

	auto context = globals::d3d::context;

	std::array<ID3D11ShaderResourceView*, 1> srvs = {
		voxelCountBuffer->srv.get()
	};

	std::array<ID3D11UnorderedAccessView*, 1> uavs = {
		voxelArgsBuffer->uav.get()
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	context->CSSetShader(voxelArgsCompute.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);

	srvs.fill(nullptr);
	uavs.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	state->EndPerfEvent();
}


bool VoxelConeTracingGI::IsValidLight(RE::BSLight* a_light)
{
	return a_light && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool VoxelConeTracingGI::IsGlobalLight(RE::BSLight* a_light)
{
	return !(a_light->portalStrict || !a_light->portalGraph);
}

void VoxelConeTracingGI::InjectLighting()
{
	auto state = globals::state;
	state->BeginPerfEvent("InjectLighting");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Light Injection");
	
	auto context = globals::d3d::context;

	lights.clear();
	lights.reserve(MAX_LIGHTS);

	auto accumulator = *globals::game::currentAccumulator.get();

	if (!settings.noIndoorDir) 
	{
		auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());
		auto sunRuntime = dirLight->GetLightRuntimeData();

		auto& directionNi = dirLight->GetWorldDirection();
		float3 directionF3 = { directionNi.x, directionNi.y, directionNi.z };
		directionF3.Normalize();

		auto diffuse = sunRuntime.diffuse;

		lights.push_back({
			.lvector = -directionF3,
			.range = 0,
			.color = { diffuse.red, diffuse.green, diffuse.blue },
			.type = 0
		});
	}

	const auto activeShadowSceneNode = accumulator->GetRuntimeData().activeShadowSceneNode;

	eastl::vector<LightLimitFix::LightData> lightsData{};
	lightsData.reserve(MAX_LIGHTS);

	auto& isl = globals::features::inverseSquareLighting;

	auto eyePosition = Util::GetEyePosition(0);

	auto addLight = [&](const RE::NiPointer<RE::BSLight>& e) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				if (IsValidLight(bsLight)) {
					auto& runtimeData = niLight->GetLightRuntimeData();

					LightLimitFix::LightData light{};
					light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
					light.lightFlags = std::bit_cast<LightLimitFix::LightFlags>(runtimeData.ambient.red);
				
					if (isl.loaded) {
						isl.ProcessLight(light, bsLight, niLight);
					} else {
						light.radius = runtimeData.radius.x;
						light.color *= runtimeData.fade;
					}

					light.color *= bsLight->lodDimmer;

					if (!IsGlobalLight(bsLight)) {
						light.lightFlags.set(LightLimitFix::LightFlags::PortalStrict);
					}

					if (bsLight->IsShadowLight()) {
						auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
						GET_INSTANCE_MEMBER(shadowLightIndex, shadowLight);
						light.shadowMaskIndex = shadowLightIndex;
						light.lightFlags.set(LightLimitFix::LightFlags::Shadow);
					}

					// Check for inactive shadow light
					if (light.shadowMaskIndex != 255) {
						/*RE::NiPoint3 worldPos;
						
						RE::TESObjectLIGH* ligh = nullptr;
						const auto refr = niLight->GetUserData();

						if (refr) {
							if (auto* objRef = refr->GetObjectReference()) {
								if (objRef->GetFormType() == RE::FormType::Light)
									ligh = objRef->As<RE::TESObjectLIGH>();
							}
						}

						auto isRef = ligh != nullptr;
						auto isOther = ligh == nullptr;
						auto isAttached = refr && !isRef && !isOther;

						if (isRef) {
							worldPos = refr->GetPosition();
						} else if (isAttached) {
							worldPos = niLight->parent->world.translate;
						} else {
							worldPos = niLight->world.translate;
						}					

						worldPos -= eyePosition;*/

						auto worldPos = niLight->world.translate - eyePosition;

						light.positionWS[0].data = float3(worldPos.x, worldPos.y, worldPos.z);

						if ((light.color.x + light.color.y + light.color.z) > 1e-4 && light.radius > 1e-4) {
							lightsData.push_back(light);
						}
					}
				}
			}
		}
	};

	const auto& activeLights = activeShadowSceneNode->GetRuntimeData().activeLights;
	for (auto& light : activeLights) {
		addLight(light);
	}

	const auto& activeShadowLights = activeShadowSceneNode->GetRuntimeData().activeShadowLights;
	for (auto& light : activeShadowLights) {
		addLight(light);
	}

	for (auto lightData: lightsData) {
		lights.push_back({ 
			.lvector = lightData.positionWS[0].data,
			.range = lightData.radius,
			.color = lightData.color,
		    .type = 1,
		});
	}

	lightBuffer->Update(lights.data(), lights.size());

	float lod0Size = static_cast<float>(settings.lod0Size) / Util::Units::GAME_UNIT_TO_M;
	float3 lod0SizeF3 = float3(lod0Size, lod0Size, lod0Size);

	uint lod0Resolution = GetLodResolution(0);
	float lod0ResolutionFloat = static_cast<float>(lod0Resolution);

	injectLightingCBData.VoxelSize = lod0SizeF3 / lod0ResolutionFloat;
	injectLightingCBData.LightCount = std::min((uint)lights.size(), MAX_LIGHTS);
	injectLightingCBData.Coord2Cell = float3(1.0f / lod0Size, 1.0f / lod0Size, 1.0f / lod0Size) * lod0ResolutionFloat;
	injectLightingCBData.Min = center - (lod0SizeF3 * 0.5);

	injectLightingCB->Update(injectLightingCBData);

	auto cb = injectLightingCB->CB();

	std::array<ID3D11ShaderResourceView*, 3> srvs = {
		lightBuffer->SRV(),
		voxelBuffer->SRV(),
		nodeBuffer->SRV()
	};

	std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			lod0Tex->uav.get()
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	context->CSSetConstantBuffers(0, 1, &cb);

	context->CSSetShader(injectLightCompute.get(), nullptr, 0);
	context->DispatchIndirect(voxelArgsBuffer->resource.get(), 0);

	context->GenerateMips(lod0Tex->srv.get());

	srvs.fill(nullptr);
	uavs.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	//context->OMSetRenderTargetsAndUnorderedAccessViews

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Debug()
{
	auto state = globals::state;
	state->BeginPerfEvent("Debug");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Debug");

	auto context = globals::d3d::context;


	auto eye = Util::GetCameraData(0);

	float2 ndcToViewMult = float2(2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1));
	float2 ndcToViewAdd = float2(-1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1));

	float2 res = globals::state->screenSize;
	float2 size = Util::ConvertToDynamic(res);

	auto lod0Resolution = GetLodResolution(0);
	float lod0ResolutionFloat = static_cast<float>(lod0Resolution);

	float lod0Size = static_cast<float>(settings.lod0Size) / Util::Units::GAME_UNIT_TO_M;
	float3 lod0SizeF3 = float3(lod0Size, lod0Size, lod0Size);

	debugCBData.NDCToView = float4(ndcToViewMult.x, ndcToViewMult.y, ndcToViewAdd.x, ndcToViewAdd.y);
	debugCBData.RcpFrameDim = float2(1.0) / size;
	debugCBData.Coord2Cell = float3(1.0f / lod0Size, 1.0f / lod0Size, 1.0f / lod0Size) * lod0ResolutionFloat;
	debugCBData.Min = center - (lod0SizeF3 * 0.5);

	debugCB->Update(debugCBData);

	auto cb = debugCB->CB();

	std::array<ID3D11ShaderResourceView*, 2> srvs = {
		prevDepth->srv.get(),
		lod0Tex->srv.get()
	};

	std::array<ID3D11UnorderedAccessView*, 1> uavs = {
		debugTex->uav.get()
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	context->CSSetConstantBuffers(0, 1, &cb);

	auto device = globals::game::renderer;
	auto screenSize = device->GetScreenSize();

    uint widthThreads = static_cast<uint>(std::ceil(screenSize.width/ 8.0f));
	uint heightThreads = static_cast<uint>(std::ceil(screenSize.height / 8.0f));

	context->CSSetShader(debugCompute.get(), nullptr, 0);
	context->Dispatch(widthThreads, heightThreads, 1);

	srvs.fill(nullptr);
	uavs.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);


	state->EndPerfEvent();
}

void VoxelConeTracingGI::Prepass()
{
	auto state = globals::state;
	state->BeginPerfEvent("VoxelConeTracingGI");

	center = float3(299, 1036, 181);

	Voxelize();
	VoxelArgs();
	InjectLighting();
	Debug();

	state->EndPerfEvent();
}

void VoxelConeTracingGI::PostPostLoad()
{
	Hooks::Install();
}


void VoxelConeTracingGI::ClearShaderCache()
{
	voxelizeCompute = nullptr;  // This is actually optional
	CompileShaders();
}

void VoxelConeTracingGI::CompileShaders()
{
	/*if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VoxelConeTracingGI\\VoxelConeTracingGI.cs.hlsl", {}, "cs_5_0")); rawPtr)
		voxelizeCompute.attach(rawPtr);*/

	CompileComputeShaders();
}

void VoxelConeTracingGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &voxelizeCompute, "VoxelizeCS.hlsl", {} },
			{ &voxelArgsCompute, "VoxelArgsCS.hlsl", {} },
			{ &injectLightCompute, "InjectLightingCS.hlsl", {} },
			{ &debugCompute, "DebugCS.hlsl", {} }
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}