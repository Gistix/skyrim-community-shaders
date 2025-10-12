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
#include "Utils/Format.h"

using namespace std::chrono;

static constexpr uint MAX_LIGHTS = 1024;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VoxelConeTracingGI::Settings,
	EnableVoxelConeTracingGI,
	NoIndoorDir,
	Lod0Resolution,
	Lod0Size,
	UpdateVoxels,
	ForceUpdate,
	VoxelDrawMode)

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
	ImGui::Checkbox("Enable Voxel Cone Tracing GI", (bool*)&settings.EnableVoxelConeTracingGI);

	ImGui::Combo("Lod0 Resolution", &settings.Lod0Resolution, dimensionsLabels, IM_ARRAYSIZE(dimensionsLabels));
	ImGui::InputInt("Lod0 Size", &settings.Lod0Size);

	if (ImGui::TreeNodeEx("Tweaks", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Disable Indoor Directional", (bool*)&settings.NoIndoorDir);
		ImGui::Checkbox("Update Voxels", (bool*)&settings.UpdateVoxels);
		ImGui::Checkbox("Force Update All Voxels", (bool*)&settings.ForceUpdate);
		ImGui::TreePop();
	}

	if (ImGui::CollapsingHeader("Debug")) {
		std::array<std::string, VoxelDrawMode::Count> items;

		const auto& itemsView = magic_enum::enum_names<VoxelDrawMode>();
		for (size_t i = 0; i < items.size(); i++) {
			items[i] = std::string(itemsView[i]);
		}

		if (ImGui::BeginCombo("Display Mode", items[settings.VoxelDrawMode].c_str())) {
			for (size_t i = 0; i < items.size(); i++) {
				bool isSelected = (settings.VoxelDrawMode == i);

				if (ImGui::Selectable(items[i].c_str(), isSelected))
					settings.VoxelDrawMode = static_cast<VoxelDrawMode>(i);

				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}
			
			ImGui::EndCombo();
		}

		if (ImGui::TreeNode("Debug Texture")) {
			ImGui::Image(debugTex->srv.get(), { debugTex->desc.Width * 0.5f, debugTex->desc.Height * 0.5f });
			ImGui::TreePop();
		}

		ImGui::Text(std::format("Queue Size: {}", queuedTriShapes.size()).c_str());

		if (ImGui::TreeNodeEx("VoxelizationHistory", ImGuiTreeNodeFlags_DefaultOpen, std::format("History ({})", history.size()).c_str())) {
			const auto num_elements = eastl::min<eastl_size_t>(5, history.size());

			if (num_elements > 0) {
				ImGui::Text(std::format("Last {} voxelization passes:", num_elements).c_str());

				if (ImGui::BeginTable("VoxelizationHistoryTable", 2, ImGuiTableFlags_Borders)) {
					const auto start_iterator = history.end() - num_elements;
					const eastl::vector<History> last_five_elements(start_iterator, history.end());

					ImGui::TableSetupColumn("Rendered TriShapes");
					ImGui::TableSetupColumn("Elapsed time");
					ImGui::TableHeadersRow();

					for (const auto element : last_five_elements) {
						ImGui::TableNextRow();

						ImGui::TableNextColumn();
						ImGui::Text(std::format("{}", element.rendered).c_str());

						ImGui::TableNextColumn();
						ImGui::Text(Util::TimeAgoString(element.time).c_str());
					}

					ImGui::EndTable();
				}

				ImGui::Text(std::format("Voxelization run count: {}", history.size()).c_str());
			} else {
				ImGui::Text("Voxelization has not ran yet.");
			}

			ImGui::TreePop();
		}
	}
}

void VoxelConeTracingGI::SetupResources()
{
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->QueryInterface(__uuidof(ID3D11Device3), device3.put_void()));

	logger::debug("Creating Constant buffers...");
	{
		voxelConeTracingGICB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VoxelConeTracingGICB>());	
		injectLightingCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<InjectLightingCB>());	
		debugCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DebugCB>());	
	}

	logger::debug("Creating Structured buffers...");
	{
		/*uint lod0MaxDepth = OctreeMaxDepth(0);
		uint lod0NodeCount = NodeCount(lod0MaxDepth);
		//uint lod0LeafNodeCount = LeafNodeCount(lod0MaxDepth);

		// Node buffer
		eastl::vector<Node> nodes(lod0NodeCount);
		nodes.at(0) = Node{ { 0, 0, 0 }, 0, 0, { 0 } };

		auto nodeBufferDesc = StructuredBufferDesc<Node>(lod0NodeCount, true, false);
		nodeBuffer = eastl::make_unique<StructuredBuffer>(nodeBufferDesc, nodes.data(), lod0NodeCount);
		nodeBuffer->CreateUAV(); // TODO: use D3D11_BUFFER_UAV_FLAG_APPEND :pray:
		nodeBuffer->CreateSRV();*/

		uint lod0Resolution = GetLodResolution(0);
		uint lod0MaxDim = lod0Resolution * lod0Resolution * lod0Resolution;

		// Voxel Append Buffer
		auto voxelAppendBufferDesc = StructuredBufferDesc<Voxel>(lod0MaxDim, true, false);
		voxelAppendBuffer = eastl::make_unique<StructuredBuffer>(voxelAppendBufferDesc, lod0MaxDim);
		voxelAppendBuffer->CreateUAV(true);
		voxelAppendBuffer->CreateSRV();

		// Voxel Draw Instance Buffer
		D3D11_BUFFER_DESC voxelDrawBufferDesc = {
			.ByteWidth = sizeof(Voxel) * lod0MaxDim,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_VERTEX_BUFFER,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		voxelDrawInstanceBuffer = eastl::make_unique<Buffer>(voxelDrawBufferDesc);

		// Voxel Buffer
		/*auto voxelBufferDesc = StructuredBufferDesc<Voxel>(lod0MaxDim, true, false);
		//eastl::vector<Voxel> voxels(lod0MaxDim);
		voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, lod0MaxDim);
		voxelBuffer->CreateUAV();
		voxelBuffer->CreateSRV();*/

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


		D3D11_BUFFER_DESC drawArgsBufferDesc = {
			.ByteWidth = 16,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
		};

		UINT args[4] = {
			36,
			0,
			0,
			0
		};

		D3D11_SUBRESOURCE_DATA drawInitData = {};
		drawInitData.pSysMem = args;

		voxelDrawIndirectArgsBuffer = eastl::make_unique<Buffer>(drawArgsBufferDesc, &drawInitData);
	}
	
	// Cube vertex buffer
	{
		float3 vertices[] = {
			// Front face
			{ 0.0f, 0.0f, 0.0f },  // 0
			{ 1.0f, 1.0f, 0.0f },  // 1
			{ 1.0f, 0.0f, 0.0f },  // 2

			{ 1.0f, 1.0f, 0.0f },  // 3
			{ 0.0f, 0.0f, 0.0f },  // 4
			{ 0.0f, 1.0f, 0.0f },  // 5

			// Back face
			{ 0.0f, 0.0f, 1.0f },  // 6
			{ 1.0f, 0.0f, 1.0f },  // 7
			{ 1.0f, 1.0f, 1.0f },  // 8

			{ 0.0f, 0.0f, 1.0f },  // 9
			{ 1.0f, 1.0f, 1.0f },  // 10
			{ 0.0f, 1.0f, 1.0f },  // 11

			// Left face
			{ 0.0f, 1.0f, 0.0f },  // 12
			{ 0.0f, 0.0f, 0.0f },  // 13
			{ 0.0f, 1.0f, 1.0f },  // 14

			{ 0.0f, 1.0f, 1.0f },  // 15
			{ 0.0f, 0.0f, 0.0f },  // 16
			{ 0.0f, 0.0f, 1.0f },  // 17

			// Right face
			{ 1.0f, 0.0f, 0.0f },  // 18
			{ 1.0f, 1.0f, 0.0f },  // 19
			{ 1.0f, 1.0f, 1.0f },  // 20

			{ 1.0f, 0.0f, 0.0f },  // 21
			{ 1.0f, 1.0f, 1.0f },  // 22
			{ 1.0f, 0.0f, 1.0f },  // 23

			// Top face
			{ 1.0f, 1.0f, 0.0f },  // 24
			{ 0.0f, 1.0f, 0.0f },  // 25
			{ 1.0f, 1.0f, 1.0f },  // 26

			{ 1.0f, 1.0f, 1.0f },  // 27
			{ 0.0f, 1.0f, 0.0f },  // 28
			{ 0.0f, 1.0f, 1.0f },  // 29

			// Bottom face
			{ 0.0f, 0.0f, 0.0f },  // 30
			{ 1.0f, 0.0f, 0.0f },  // 31
			{ 1.0f, 0.0f, 1.0f },  // 32

			{ 0.0f, 0.0f, 1.0f },  // 33
			{ 0.0f, 0.0f, 0.0f },  // 34
			{ 1.0f, 0.0f, 1.0f },  // 35
		};

		D3D11_BUFFER_DESC cubeVertexDesc = {};
		cubeVertexDesc.Usage = D3D11_USAGE_DEFAULT;
		cubeVertexDesc.ByteWidth = sizeof(vertices);
		cubeVertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		cubeVertexDesc.CPUAccessFlags = 0;

		D3D11_SUBRESOURCE_DATA cubeVertexInit = {};
		cubeVertexInit.pSysMem = vertices;

		voxelDrawVertexBuffer = eastl::make_unique<Buffer>(cubeVertexDesc, &cubeVertexInit);

		voxelDrawInputDesc = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "VOXELCOORD", 0, DXGI_FORMAT_R32G32B32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELALBEDO", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Albedo), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELEMISSION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Emission), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELNORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Normal), D3D11_INPUT_PER_INSTANCE_DATA, 1 }
		};
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
				.MiscFlags = 0 // D3D11_RESOURCE_MISC_GENERATE_MIPS
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
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
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

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MipSlice = 0 
				}
			};

			debugTex = eastl::make_unique<Texture2D>(texDesc); 
			debugTex->CreateSRV(srvDesc);
			debugTex->CreateUAV(uavDesc);
			debugTex->CreateRTV(rtvDesc);
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
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
		DX::ThrowIfFailed(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)));
		logger::info("CheckFeatureSupport: ConservativeRasterizationTier: {}", magic_enum::enum_name(options2.ConservativeRasterizationTier));

		D3D11_RASTERIZER_DESC2 rasterDesc = {};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.FrontCounterClockwise = FALSE;
		rasterDesc.DepthBias = 0;
		rasterDesc.DepthBiasClamp = 0.0f;
		rasterDesc.SlopeScaledDepthBias = 0.0f;
		rasterDesc.DepthClipEnable = FALSE;
		rasterDesc.ScissorEnable = TRUE;
		rasterDesc.MultisampleEnable = FALSE;
		rasterDesc.AntialiasedLineEnable = FALSE;
		rasterDesc.ForcedSampleCount = 0;
		rasterDesc.ConservativeRaster = options2.ConservativeRasterizationTier == D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED ? D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF : D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;

		auto hr = device3->CreateRasterizerState2(&rasterDesc, voxelRasterState.put());

		if (FAILED(hr)) {
			logger::error("CreateRasterizerState2: Failed to create D3D11.3 ID3D11RasterizerState2, HRESULT: 0x{:08X}", static_cast<uint32_t>(hr));
			DX::ThrowIfFailed(hr);
		}
	}

	// Viewport
	{
		float lod0ResFloat = static_cast<float>(GetLodResolution(0));

		lod0Viewport.TopLeftX = 0.0f;
		lod0Viewport.TopLeftY = 0.0f;
		lod0Viewport.Width = lod0ResFloat;
		lod0Viewport.Height = lod0ResFloat;
		lod0Viewport.MinDepth = 0.0f;
		lod0Viewport.MaxDepth = 1.0f;
	}

	// Depth Stencil State
	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, voxelDrawDSS.put()));
	}

	CompileShaders();
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

void VoxelConeTracingGI::BSShader_SetupGeometry(RE::BSShader* This, RE::BSRenderPass* Pass)
{
	logger::debug(
		"BSShader_SetupGeometry - Shader: [0x{:x}], Pass: [0x{:x}], ShaderType: {}",
		reinterpret_cast<uintptr_t>(This),
		reinterpret_cast<uintptr_t>(Pass),
		magic_enum::enum_name(This->shaderType.get()));

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();
	const auto rendererData = triShape->GetGeometryRuntimeData().rendererData;

	if (rendererData == nullptr)
		return;

	auto it = cachedTriShapes.find(rendererData);

	// TriShape not cached, lets add to cache and queue
	if (it == cachedTriShapes.end()) {
		cachedTriShapes.emplace(rendererData);
		queuedTriShapes.emplace(rendererData);
	} else if (settings.ForceUpdate == TRUE) {
		queuedTriShapes.emplace(rendererData);
	}
}

void VoxelConeTracingGI::BSShader_RestoreGeometry(RE::BSShader* This, RE::BSRenderPass* Pass)
{
	logger::debug(
		"BSShader_RestoreGeometry - Shader: [0x{:x}], Pass: [0x{:x}], ShaderType: {}",
		reinterpret_cast<uintptr_t>(This),
		reinterpret_cast<uintptr_t>(Pass),
		magic_enum::enum_name(This->shaderType.get()));

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();
	const auto triShapeRuntime = triShape->GetTrishapeRuntimeData();
	const auto rendererData = triShape->GetGeometryRuntimeData().rendererData;

	logger::debug(
		"BSLightingShader_RestoreGeometry - Geometry: [0x{:x}], BSTriShape: [0x{:x}], BSGraphics::TriShape: [0x{:x}]",
		reinterpret_cast<uintptr_t>(geometry),
		reinterpret_cast<uintptr_t>(triShape),
		reinterpret_cast<uintptr_t>(rendererData)
	);

	if (rendererData == nullptr)
		return;

	auto it = queuedTriShapes.find(rendererData);

	// TriShape not queued
	if (it == queuedTriShapes.end())
		return;

	auto it2 = cachedTriShapes.find(rendererData);

	// TriShape not cached
	if (it2 == cachedTriShapes.end())
		return;

	/*float4x4 xmmTransform = GetXMFromNiTransform(triShape->world);
	float4x4 transform = xmmTransform.Transpose();

	//for (uint row = 0; row < 3; ++row) {
	//	for (uint col = 0; col < 4; ++col) {
	//		transform[row * 4 + col] = xmmTransform.m[col][row];
	//	}
	//}

	const auto& modelData = triShape->GetModelData().modelBound;

	const RE::NiPoint3 r = { modelData.radius, modelData.radius, modelData.radius };

	const RE::NiPoint3 aabbMin = modelData.center - r;
	const RE::NiPoint3 aabbMax = modelData.center + r;*/

	// Render here
	auto context = globals::d3d::context;

	auto state = globals::state;
	state->BeginPerfEvent("Voxelization");

	ID3D11VertexShader* prevVertex = nullptr;
	ID3D11PixelShader* prevPixel = nullptr;

	context->VSGetShader(&prevVertex, nullptr, nullptr);
	context->PSGetShader(&prevPixel, nullptr, nullptr);

	context->VSSetShader(voxelizeVertex.get(), nullptr, 0);
	context->GSSetShader(voxelizeGeometry.get(), nullptr, 0);
	context->PSSetShader(voxelizePixel.get(), nullptr, 0);

	context->RSSetState(voxelRasterState.get());

	// Lets back everything up
	uint numViewports = 1;
	D3D11_VIEWPORT prevViewport = {};
	context->RSGetViewports(&numViewports, &prevViewport);

	ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

	// Overide
	context->RSSetViewports(1, &lod0Viewport);

	ID3D11UnorderedAccessView* uav = voxelAppendBuffer->UAV();
	context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &uav, nullptr);

	// Render
	context->DrawIndexed(triShapeRuntime.triangleCount * 3, 0, 0);

	// Restore
	context->VSSetShader(prevVertex, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(prevPixel, nullptr, 0);

	context->RSSetViewports(1, &prevViewport);

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);

	if (prevVertex)
		prevVertex->Release();

	if (prevPixel)
		prevPixel->Release();

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i])
			prevRTVs[i]->Release();
	}

	if (prevDSV)
		prevDSV->Release();

	// Just rendered, remove from queue
	queuedTriShapes.erase(rendererData);
	rendered++;

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Main_RenderPlayerView(void* a1, bool a2, bool a3)
{
	Hooks::Main_RenderPlayerView::func(a1, a2, a3);
}


void VoxelConeTracingGI::Main_RenderWorld(bool a1)
{
	if (queuedReset) {
		cachedTriShapes.clear();
		queuedTriShapes.clear();
		history.clear();

		queuedReset = false;
	}

	rendered = 0;

	float lod0Size = static_cast<float>(settings.Lod0Size) / Util::Units::GAME_UNIT_TO_M;

	uint lod0Resolution = GetLodResolution(0);
	float lod0ResolutionFloat = static_cast<float>(lod0Resolution);

	voxelConeTracingGICBData.Min = center - (float3(lod0Size, lod0Size, lod0Size) * 0.5f);
	voxelConeTracingGICBData.Size = lod0Size;
	voxelConeTracingGICBData.SizeInv = 1.0f / lod0Size;
	voxelConeTracingGICBData.Res = lod0Resolution;
	voxelConeTracingGICBData.ResInv = 1.0f / lod0ResolutionFloat;
	voxelConeTracingGICBData.VoxelSize = lod0Size / lod0ResolutionFloat;

	voxelConeTracingGICB->Update(voxelConeTracingGICBData);

	auto cb = voxelConeTracingGICB->CB();

	auto context = globals::d3d::context;

	context->GSSetConstantBuffers(0, 1, &cb);

	Hooks::Main_RenderWorld::func(a1);

	if (rendered > 0) {
		history.push_back({ steady_clock::now(), rendered });
	}

	Main_RenderWorldAfter();
}

void VoxelConeTracingGI::BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data)
{
	const auto rendererData = This->GetGeometryRuntimeData().rendererData;

	// TriShape not cached, already queued or updates turned off
	if (cachedTriShapes.find(rendererData) == cachedTriShapes.end() || queuedTriShapes.find(rendererData) != queuedTriShapes.end() || settings.UpdateVoxels == false) {
		Hooks::BSTriShape_UpdateWorldData::func(This, a_data);
	} else {
		RE::NiPoint3 pointA = This->world * RE::NiPoint3{ 1.0f, 1.0f, 1.0f };

		Hooks::BSTriShape_UpdateWorldData::func(This, a_data);

		RE::NiPoint3 pointB = This->world * RE::NiPoint3{ 1.0f, 1.0f, 1.0f };

		if (pointA.GetDistance(pointB) > 0.1f) {
			// Lets queue the TriShape for rendering again
			queuedTriShapes.emplace(rendererData);
		}
	}
}

VoxelConeTracingGI::VoxelConeTracingGICB VoxelConeTracingGI::GetCommonBufferData() const
{
	return voxelConeTracingGICBData;
}

void VoxelConeTracingGI::Main_RenderWorldAfter()
{
	if (settings.VoxelDrawMode != VoxelDrawMode::None && settings.VoxelDrawMode != VoxelDrawMode::Count)
		DrawVoxels();
}

void VoxelConeTracingGI::Voxelize()
{
	auto state = globals::state;
	state->BeginPerfEvent("Voxelize");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Voxelize");

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

	if (!settings.NoIndoorDir) 
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

	float lod0Size = static_cast<float>(settings.Lod0Size) / Util::Units::GAME_UNIT_TO_M;
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

	state->EndPerfEvent();
}

void VoxelConeTracingGI::DrawVoxels()
{
	auto state = globals::state;
	state->BeginPerfEvent("Draw Voxels");

	auto context = globals::d3d::context;

	// Lets back everything up
	ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

	/*ID3D11VertexShader* prevVertex = nullptr;
	ID3D11PixelShader* prevPixel = nullptr;

	context->VSGetShader(&prevVertex, nullptr, nullptr);
	context->PSGetShader(&prevPixel, nullptr, nullptr);*/

	auto cb = voxelConeTracingGICB->CB();
	context->VSSetConstantBuffers(0, 1, &cb);

	context->VSSetShader(drawVertex.get(), nullptr, 0);

	auto drawPixel = drawPixelVariants[settings.VoxelDrawMode - 1];
	context->PSSetShader(drawPixel.get(), nullptr, 0);

	auto renderer = globals::game::renderer;
	auto screenSize = renderer->GetScreenSize();

	D3D11_VIEWPORT viewport = {
		.TopLeftX = 0.0f,
		.TopLeftY = 0.0f,
		.Width = static_cast<float>(screenSize.width),
		.Height = static_cast<float>(screenSize.height),
		.MinDepth = 0.0f,
		.MaxDepth = 1.0f
	};
	context->RSSetViewports(1, &viewport);

	context->OMSetDepthStencilState(voxelDrawDSS.get(), 0);

	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

	//auto rtv = debugTex->rtv.get();
	//context->OMSetRenderTargets(1, &rtv, nullptr);

	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto depthView = mainDepth.views[0];

	// Clear Color and Depth
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	context->ClearRenderTargetView(main.RTV, clearColor);
	context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	
	context->OMSetRenderTargets(1, &main.RTV, depthView);

	context->IASetInputLayout(voxelDrawInputLayout.get());

	UINT strides[2] = { sizeof(float3), sizeof(Voxel) };  // size of one vertex and one instance
	UINT offsets[2] = { 0, 0 };

	context->CopyResource(voxelDrawInstanceBuffer->resource.get(), voxelAppendBuffer->resource.get());
	ID3D11Buffer* buffers[2] = { voxelDrawVertexBuffer->resource.get(), voxelDrawInstanceBuffer->resource.get() };

	context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
	//context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context->CopyStructureCount(voxelDrawIndirectArgsBuffer->resource.get(), 4, voxelAppendBuffer->UAV());

	context->DrawInstancedIndirect(voxelDrawIndirectArgsBuffer->resource.get(), 0);

	/*context->VSSetShader(prevVertex, nullptr, 0);
	context->PSSetShader(prevPixel, nullptr, 0);

	if (prevVertex)
		prevVertex->Release();

	if (prevPixel)
		prevPixel->Release();*/

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i])
			prevRTVs[i]->Release();
	}

	if (prevDSV)
		prevDSV->Release();

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Debug()
{
	auto state = globals::state;
	state->BeginPerfEvent("Debug");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Debug");

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Prepass()
{
	if (!settings.EnableVoxelConeTracingGI)
		return;

	auto state = globals::state;
	state->BeginPerfEvent("VoxelConeTracingGI");

	center = float3(299, 1036, 181);

	/*Voxelize();
	VoxelArgs();
	InjectLighting();*/
	//Debug();

	//UINT values[4] = { 0, 0, 0, 0 };
	//globals::d3d::context->ClearUnorderedAccessViewUint(voxelAppendBuffer->UAV(), values);

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
	auto compileShader = [&]<typename T>(winrt::com_ptr<T>* programPtr, const std::filesystem::path& folderPath, std::string_view filename, const char* programType, std::vector<std::pair<const char*, const char*>> defines = {}, const std::vector<D3D11_INPUT_ELEMENT_DESC>& inputDesc = {}, winrt::com_ptr<ID3D11InputLayout>* inputLayoutPtr = nullptr) {
		auto path = folderPath / filename;
		
		if (auto rawPtr = reinterpret_cast<T*>(Util::CompileShader(path.c_str(), defines, programType, "main", inputDesc, inputLayoutPtr ? inputLayoutPtr->put() : nullptr)))
			programPtr->attach(rawPtr);	
	};

	auto folderPath = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI");

	auto voxelizationFolderpath = folderPath / "Voxelization";
	compileShader(&voxelizeVertex, voxelizationFolderpath , "VoxelizeVS.hlsl", "vs_5_0");
	compileShader(&voxelizeGeometry, voxelizationFolderpath , "VoxelizeGS.hlsl", "gs_5_0");
	compileShader(&voxelizePixel, voxelizationFolderpath , "VoxelizePS.hlsl", "ps_5_0");

	auto debugFolderpath = folderPath / "Debug";
	compileShader(&drawVertex, debugFolderpath, "DrawVS.hlsl", "vs_5_0", {}, voxelDrawInputDesc, &voxelDrawInputLayout);

	for (size_t i = 0; i < drawPixelVariants.size(); i++) {
		compileShader(&drawPixelVariants[i], debugFolderpath, "DrawPS.hlsl", "ps_5_0", { { "DRAW_MODE", std::to_string(i).c_str() } });
	}

	compileShader(&voxelizeCompute, folderPath , "VoxelizeCS.hlsl", "cs_5_0");
	compileShader(&voxelArgsCompute, folderPath , "VoxelArgsCS.hlsl", "cs_5_0");
	compileShader(&injectLightCompute, folderPath , "InjectLightingCS.hlsl", "cs_5_0");
	compileShader(&debugCompute, folderPath , "DebugCS.hlsl", "cs_5_0");
}

RE::BSEventNotifyControl VoxelConeTracingGI::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell through a loadscreen, update every frame until completion
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			globals::features::voxelConeTracingGI.queuedReset = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}