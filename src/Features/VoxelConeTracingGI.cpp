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
#include "ShaderCache.h"

using namespace std::chrono;

static constexpr uint MAX_LIGHTS = 1024;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VoxelConeTracingGI::Settings,
	EnableVoxelConeTracingGI,
	NoIndoorDir,
	Resolution,
	Size,
	ClipmapCount,
	ScaleFactor,
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

	{
		auto find = [](auto& array, uint value) -> int {
			for (size_t i = 0; i < std::size(array); ++i) {
				if (array[i] == value) {
					return static_cast<int>(i);
				}
			}

			return 0;
		};

		int resolution = find(resolutions, settings.Resolution); 

		if (ImGui ::Combo("Resolution", &resolution, resolutionsLabels, IM_ARRAYSIZE(resolutionsLabels))) {
			settings.Resolution = resolutions[resolution];

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Volume resolution per axis\n");
			}
		}
	}

	ImGui::InputInt("Size", &settings.Size);
	settings.Size = std::max(1, settings.Size);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("World size of first volume\n");
	}

	ImGui::SliderInt("Clipmaps", &settings.ClipmapCount, 2, 8);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Number of clipmaps to use\n");
	}

	{
		ImGui::SliderInt("Clipmap Scale Factor", &settings.ScaleFactor, 2, 10);

		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("The world space size multiplier for each subsequent clipmap\n");
		}

		const auto& clipmapCount = settings.ClipmapCount;

		std::string label;

		for (int i = 0; i < clipmapCount; ++i) {
			label += std::format("Clip{} Size: {}", i, settings.Size * std::pow(settings.ScaleFactor, i));

			if (i < clipmapCount-1) {
				label += ", ";
			}
		}

		ImGui::Text(label.c_str());
	}
	
	if (ImGui::TreeNodeEx("Tweaks", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Optional tweaks\n");
		}

		ImGui::Checkbox("Disable Indoor Directional", (bool*)&settings.NoIndoorDir);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Disables directional lights when indoors for better direct and indirect lighting\n");
		}

		//ImGui::Checkbox("Update Voxels", (bool*)&settings.UpdateVoxels);
		//ImGui::Checkbox("Force Update All Voxels", (bool*)&settings.ForceUpdate);
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

		/*if (ImGui::TreeNode("Debug Texture")) {
			ImGui::Image(debugTex->srv.get(), { debugTex->desc.Width * 0.5f, debugTex->desc.Height * 0.5f });
			ImGui::TreePop();
		}*/

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
		vsFrameBufferPSCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VSFrameBufferPS>());	
	}

	uint resolution = settings.Resolution;
	uint maxVoxelCount = resolution * resolution * resolution;

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

		// Voxel Append Buffer
		auto voxelSamplesBufferDesc = StructuredBufferDesc<Voxel>(maxVoxelCount, true, false);
		voxelSamplesBuffer = eastl::make_unique<StructuredBuffer>(voxelSamplesBufferDesc, maxVoxelCount);
		voxelSamplesBuffer->CreateUAV(true);
		voxelSamplesBuffer->CreateSRV();
		
		// Voxel Draw Instance Buffer
		D3D11_BUFFER_DESC voxelDrawBufferDesc = {
			.ByteWidth = sizeof(Voxel) * maxVoxelCount,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		voxelDrawInstanceBuffer = eastl::make_unique<Buffer>(voxelDrawBufferDesc);
		voxelDrawInstanceBuffer->CreateUAV();

		// Voxel Buffer
		auto voxelBufferDesc = StructuredBufferDesc<Voxel>(maxVoxelCount, true, false);
		//voxelBufferDesc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
		//eastl::vector<Voxel> voxels(maxVoxelCount);
		voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, maxVoxelCount);
		voxelBuffer->CreateUAV(true);
		voxelBuffer->CreateSRV();

		// Light buffer
		auto lightBufferDesc = StructuredBufferDesc<Light>(MAX_LIGHTS, false, true);
		lightBuffer = eastl::make_unique<StructuredBuffer>(lightBufferDesc, MAX_LIGHTS);
		lightBuffer->CreateSRV();
	}

	logger::debug("Creating Raw buffers...");
	{
		uint numBits = maxVoxelCount;
		uint numBytes = (numBits + 7) / 8;
		uint maskElements = (numBytes + 3) / 4;

		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = 4 * maskElements,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = { 
				.FirstElement = 0,
				.NumElements = maskElements,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW
			}
		};
		
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,  // DXGI_FORMAT_R32_UINT
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX,
			.BufferEx = {
				.FirstElement = 0,
				.NumElements = maskElements,
				.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW
			}
		};

		// Multi purpose indirect arguments buffer
		UINT indirectArgsInit[11] = {
			// Used to draw voxels to the screen
			36, // 0 - Vertices (Cube)
			0,  // 1 - Instance Count (Voxel Count)
			0,	// 2
			0,  // 3

			// Used to draw samples to the texture
			0,  // 4 - Vertices (Voxel Sample Count)
			1,  // 5 - Instance Count
			0,  // 6
			0,  // 7

			// Used to inject lighting into voxels
			0,  // 8 - ThreadGroupCountX (Voxel Count / 64)
			1,  // 9 - ThreadGroupCountY
			1   // 10 - ThreadGroupCountZ
		};

		uint indirectArgsByteWidth = sizeof(indirectArgsInit);

		D3D11_BUFFER_DESC indirectArgsBufferDesc = {
			.ByteWidth = indirectArgsByteWidth,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
		};

		D3D11_SUBRESOURCE_DATA indirectArgsInitData = {};
		indirectArgsInitData.pSysMem = indirectArgsInit;

		D3D11_UNORDERED_ACCESS_VIEW_DESC indirectArgsUAVDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = indirectArgsByteWidth / sizeof(indirectArgsInit[0]),
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW 
			}
		};

		voxelIndirectArgsBuffer = eastl::make_unique<Buffer>(indirectArgsBufferDesc, &indirectArgsInitData);
		voxelIndirectArgsBuffer->CreateUAV(indirectArgsUAVDesc);
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
	}

	// Input Description
	{
		voxelInputDesc = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "VOXELCOORD", 0, DXGI_FORMAT_R32G32B32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELALBEDO", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Albedo), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELNORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Normal), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			{ "VOXELEMISSION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Voxel, Voxel::Emissive), D3D11_INPUT_PER_INSTANCE_DATA, 1 }
		};
		
		accumulateInputDesc = {
			{ "VOXELCOORD", 0, DXGI_FORMAT_R32G32B32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "VOXELALBEDO", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Voxel, Voxel::Albedo), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "VOXELNORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Voxel, Voxel::Normal), D3D11_INPUT_PER_VERTEX_DATA, 0 }, 
			{ "VOXELEMISSIVE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Voxel, Voxel::Emissive), D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
	}

	logger::debug("Creating textures...");
	{
		// Volume texture
		{
			D3D11_TEXTURE3D_DESC lod0TexDesc{
				.Width = resolution,
				.Height = resolution,
				.Depth = resolution,
				.MipLevels = 0,
				.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
				.CPUAccessFlags = 0,
				.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS
			};

			D3D11_SHADER_RESOURCE_VIEW_DESC lod0SRVDesc = {
				.Format = lod0TexDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
				.Texture3D = {
					.MostDetailedMip = 0,
					.MipLevels = ~0u
				}
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC lod0UAVDesc = {
				.Format = lod0TexDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
				.Texture3D = {
					.MipSlice = 0,
					.FirstWSlice = 0,
					.WSize = ~0u
				}
			};

			lod0Tex = eastl::make_unique<Texture3D>(lod0TexDesc);
			lod0Tex->CreateSRV(lod0SRVDesc);
			lod0Tex->CreateUAV(lod0UAVDesc);

			D3D11_TEXTURE2D_DESC texDesc = {
				.Width = resolution * resolution,
				.Height = resolution,
				.MipLevels = 1,
				.ArraySize = 3, // Albedo, Emission and Normal
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
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = {
					.MostDetailedMip = 0,
					.MipLevels = 1,
					.FirstArraySlice = 0,
					.ArraySize = 1 
				}
			};

			/*D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = {
					.MipSlice = 0,
					.FirstArraySlice = 0,
					.ArraySize = 1 
				}
			};*/

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = {
					.MipSlice = 0,
					.FirstArraySlice = 0,
					.ArraySize = 1 
				}
			};

			voxelAccumulator = eastl::make_unique<Texture2DArray>(texDesc);
			voxelAccumulator->CreateSRV(srvDesc);
			voxelAccumulator->CreateRTV(rtvDesc);

			srvDesc.Texture2DArray.FirstArraySlice = 1;
			rtvDesc.Texture2DArray.FirstArraySlice = 1;
			voxelAccumulator->CreateSRV(srvDesc);
			voxelAccumulator->CreateRTV(rtvDesc);

			srvDesc.Texture2DArray.FirstArraySlice = 2;
			rtvDesc.Texture2DArray.FirstArraySlice = 2;
			voxelAccumulator->CreateSRV(srvDesc);
			voxelAccumulator->CreateRTV(rtvDesc);
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
			debugTex->CreateSRV(srvDesc); // crashing here
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
		rasterDesc.FrontCounterClockwise = TRUE;
		rasterDesc.DepthBias = 0;
		rasterDesc.DepthBiasClamp = 0.0f;
		rasterDesc.SlopeScaledDepthBias = 0.0f;
		rasterDesc.DepthClipEnable = FALSE;
		rasterDesc.ScissorEnable = FALSE;
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

	// Blend States
	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.IndependentBlendEnable = TRUE;

		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		
		blendDesc.RenderTarget[1].BlendEnable = TRUE;
		blendDesc.RenderTarget[1].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[1].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[1].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		
		blendDesc.RenderTarget[2].BlendEnable = TRUE;
		blendDesc.RenderTarget[2].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[2].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[2].BlendOp = D3D11_BLEND_OP_MAX;
		blendDesc.RenderTarget[2].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[2].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[2].BlendOpAlpha = D3D11_BLEND_OP_MAX;
		blendDesc.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		device->CreateBlendState(&blendDesc, accumulateBlendState.put());
	}

	// Viewport
	{
		float lod0ResFloat = static_cast<float>(settings.Resolution);

		lod0Viewport.TopLeftX = 0.0f;
		lod0Viewport.TopLeftY = 0.0f;
		lod0Viewport.Width = lod0ResFloat;
		lod0Viewport.Height = lod0ResFloat;
		lod0Viewport.MinDepth = 0.0f;
		lod0Viewport.MaxDepth = 1.0f;
	
		accumulateViewport.TopLeftX = 0.0f;
		accumulateViewport.TopLeftY = 0.0f;
		accumulateViewport.Width = lod0ResFloat * lod0ResFloat;
		accumulateViewport.Height = lod0ResFloat;
		accumulateViewport.MinDepth = 0.0f;
		accumulateViewport.MaxDepth = 1.0f;
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
	if (!renderingWorld)
		return;

	const auto shaderType = This->shaderType.get();

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();

	if (triShape == nullptr) {
		logger::warn(
			"BSShader_SetupGeometry - nullptr triShape for Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}",
			reinterpret_cast<uintptr_t>(Pass),
			reinterpret_cast<uintptr_t>(This),
			magic_enum::enum_name(shaderType),
			reinterpret_cast<uintptr_t>(geometry),
			geometry->name.c_str());
			
		return;
	}

	/*logger::info(
		"BSShader_SetupGeometry - Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}, TriShape: 0x{:x}",
		reinterpret_cast<uintptr_t>(Pass),
		reinterpret_cast<uintptr_t>(This),
		magic_enum::enum_name(shaderType),
		reinterpret_cast<uintptr_t>(geometry),
		geometry->name.c_str(),
		reinterpret_cast<uintptr_t>(triShape));*/

	auto it = cachedTriShapes.find(triShape);

	// TriShape not cached, lets add to cache and queue
	if (it == cachedTriShapes.end()) {
		cachedTriShapes.emplace(triShape);
		queuedTriShapes.emplace(triShape);
	} else if (settings.ForceUpdate == TRUE) {
		queuedTriShapes.emplace(triShape);
	}
}

void VoxelConeTracingGI::BSShader_RestoreGeometry(RE::BSShader* This, RE::BSRenderPass* Pass)
{
	if (!renderingWorld)
		return;

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();

	if (triShape == nullptr)
		return;

	const auto shaderType = This->shaderType.get();

	// Vertex Shader
	auto vs = voxelizeVertex.find(shaderType);

	if (vs == voxelizeVertex.end())
		return;

	auto vertexShaderPermutations = vs->second;

	// Pixel Shader
	auto ps = voxelizePixel.find(shaderType);

	if (ps == voxelizePixel.end())
		return;

	auto pixelShaderPermutations = ps->second;

	auto state = globals::state;

	auto vertexDescriptor = state->currentVertexDescriptor;
	//auto pixelDescriptor = state->currentPixelDescriptor;

	std::map<std::string, std::string> shaderDefines;
	for (const auto& item: GetShaderDefines<std::string>(shaderType, vertexDescriptor)) {
		shaderDefines.emplace(item.first, item.second);
	};

	auto vertexShader = vertexShaderPermutations.Get(shaderDefines);
	
	if (vertexShader == nullptr)
		return;

	auto pixelShader = pixelShaderPermutations.Get(shaderDefines);

	if (pixelShader == nullptr)
		return;

	auto it = queuedTriShapes.find(triShape);

	// TriShape not queued
	if (it == queuedTriShapes.end())
		return;

	auto it2 = cachedTriShapes.find(triShape);

	// TriShape not cached
	if (it2 == cachedTriShapes.end())
		return;

	const auto triShapeRuntime = triShape->GetTrishapeRuntimeData();

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

	state->BeginPerfEvent("Voxelization");

	ID3D11VertexShader* prevVertex = nullptr;
	ID3D11ClassInstance* prevVertexClassInstances[256] = {};
    UINT prevNumVertexClassInstances = 256;

	ID3D11PixelShader* prevPixel = nullptr;
	ID3D11ClassInstance* prevPixelClassInstances[256] = {};
	UINT prevNumPixelClassInstances = 256;

	context->VSGetShader(&prevVertex, prevVertexClassInstances, &prevNumVertexClassInstances);
	context->PSGetShader(&prevPixel, prevPixelClassInstances, &prevNumPixelClassInstances);

	ID3D11RasterizerState* prevState = nullptr;
	context->RSGetState(&prevState);

	/*UINT numViewports = 0;
	context->RSGetViewports(&numViewports, nullptr);

	std::vector<D3D11_VIEWPORT> viewports(numViewports);
	context->RSGetViewports(&numViewports, viewports.data());*/

	// Lets back everything up
	uint numViewports = 1;
	D3D11_VIEWPORT prevViewport = {};
	context->RSGetViewports(&numViewports, &prevViewport);

	//std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> prevRTVs;
	ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

	/*ID3D11BlendState* prevBlendState = nullptr;
	float prevBlendFactor[4] = {};
	UINT prevBlendMask = 0;
	context->OMGetBlendState(&prevBlendState, prevBlendFactor, &prevBlendMask);*/

	context->VSSetShader(vertexShader.get(), nullptr, 0);
	context->GSSetShader(voxelizeGeometry.get(), nullptr, 0);
	context->PSSetShader(pixelShader.get(), nullptr, 0);

	context->RSSetState(voxelRasterState.get());

	context->RSSetViewports(1, &lod0Viewport);

	std::array<ID3D11UnorderedAccessView*, 1> uavs = {
		voxelSamplesBuffer->UAV()
	};

	context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, (uint)uavs.size(), uavs.data(), nullptr);

	/*float blendFactor[4] = { 0, 0, 0, 0 };
	context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);*/

	// Render
	context->DrawIndexed(triShapeRuntime.triangleCount * 3, 0, 0);

	// Restore
	context->VSSetShader(prevVertex, prevVertexClassInstances, prevNumVertexClassInstances);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(prevPixel, prevPixelClassInstances, prevNumPixelClassInstances);

	if (prevVertex)
		prevVertex->Release();

	for (UINT i = 0; i < prevNumVertexClassInstances; ++i) {
		if (prevVertexClassInstances[i])
			prevVertexClassInstances[i]->Release();
	}

	for (UINT i = 0; i < prevNumPixelClassInstances; ++i) {
		if (prevPixelClassInstances[i])
			prevPixelClassInstances[i]->Release();
	}

	if (prevPixel)
		prevPixel->Release();

	context->RSSetState(prevState);

	if (prevState)
		prevState->Release();

	context->RSSetViewports(1, &prevViewport);

	uavs.fill(nullptr);

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);
	//context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs.data(), prevDSV, prevUAVSlot, prevNumUAVs, prevUAVs, nullptr);

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i])
			prevRTVs[i]->Release();
	}

	if (prevDSV)
		prevDSV->Release();

	/*context->OMSetBlendState(prevBlendState, prevBlendFactor, prevBlendMask);

	if (prevBlendState)
		prevBlendState->Release();*/

	// Just rendered, remove from queue
	queuedTriShapes.erase(triShape);
	rendered++;

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Main_RenderPlayerView(void* a1, bool a2, bool a3)
{
	Hooks::Main_RenderPlayerView::func(a1, a2, a3);
}


void VoxelConeTracingGI::Main_RenderWorld(bool a1)
{
	bool enabled = Enabled();

	if (enabled) {
		if (queuedReset) {
			cachedTriShapes.clear();
			queuedTriShapes.clear();
			history.clear();

			queuedReset = false;
		}

		rendered = 0;

		vsFrameBufferPSData.CameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
		vsFrameBufferPSCB->Update(vsFrameBufferPSData);
		auto vsFBPSCB = vsFrameBufferPSCB->CB();

		float size = static_cast<float>(settings.Size) / Util::Units::GAME_UNIT_TO_M;

		uint resolution = settings.Resolution;
		float resolutionFloat = static_cast<float>(resolution);

		voxelConeTracingGICBData.Min = center - (float3(size, size, size) * 0.5f);
		voxelConeTracingGICBData.Size = size;
		voxelConeTracingGICBData.SizeInv = 1.0f / size;
		voxelConeTracingGICBData.Res = resolution;
		voxelConeTracingGICBData.ResInv = 1.0f / resolutionFloat;
		voxelConeTracingGICBData.VoxelSize = size / resolutionFloat;
		voxelConeTracingGICB->Update(voxelConeTracingGICBData);
		auto vxGICB = voxelConeTracingGICB->CB();

		auto context = globals::d3d::context;
		
		context->VSSetConstantBuffers(13, 1, &vsFBPSCB);
		context->GSSetConstantBuffers(0, 1, &vxGICB);

		renderingWorld = true;
	}

	Hooks::Main_RenderWorld::func(a1);

	if (enabled) {
		renderingWorld = false;

		if (rendered > 0) {
			const auto& record = History(steady_clock::now(), rendered);

			if (history.size() < MAX_HISTORY) {
				history.push_back(record);
			} else {
				history.erase_first(history.begin());
				history.push_back(record);
			}
		}

		Main_RenderWorldAfter();
	}
}

void VoxelConeTracingGI::BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data)
{
	// TriShape not cached, already queued or updates turned off
	if (cachedTriShapes.find(This) == cachedTriShapes.end() || queuedTriShapes.find(This) != queuedTriShapes.end() || !Enabled() || !Update()) {
		Hooks::BSTriShape_UpdateWorldData::func(This, a_data);
	} else {
		RE::NiPoint3 pointA = This->world * RE::NiPoint3{ 1.0f, 1.0f, 1.0f };

		Hooks::BSTriShape_UpdateWorldData::func(This, a_data);

		RE::NiPoint3 pointB = This->world * RE::NiPoint3{ 1.0f, 1.0f, 1.0f };

		if (pointA.GetDistance(pointB) > 0.1f) {
			// Lets queue the TriShape for rendering again
			queuedTriShapes.emplace(This);
		}
	}
}

VoxelConeTracingGI::VoxelConeTracingGICB VoxelConeTracingGI::GetCommonBufferData() const
{
	return voxelConeTracingGICBData;
}

void VoxelConeTracingGI::Main_RenderWorldAfter()
{
	auto state = globals::state;
	state->BeginPerfEvent("VXGI - WorldAfter");

	PostProcess();

	InjectLighting();

	if (settings.VoxelDrawMode != VoxelDrawMode::None && settings.VoxelDrawMode != VoxelDrawMode::Count) {
		DrawVoxels();
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::PostProcess()
{
	auto state = globals::state;
	state->BeginPerfEvent("Post-Process");

	auto context = globals::d3d::context;

	// Write samples to RTVs
	{
		state->BeginPerfEvent("Blend samples to RTV");

		// Copy sample count to [7]
		context->CopyStructureCount(voxelIndirectArgsBuffer->resource.get(), 4 * 4, voxelSamplesBuffer->UAV());

		std::array<ID3D11RenderTargetView*, 3> rtvs = {
			voxelAccumulator->RTV(0),
			voxelAccumulator->RTV(1),
			voxelAccumulator->RTV(2)
		};

		float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(rtvs[0], clearColor);
		context->ClearRenderTargetView(rtvs[1], clearColor);
		context->ClearRenderTargetView(rtvs[2], clearColor);

		context->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), nullptr);

		context->RSSetViewports(1, &accumulateViewport);

		context->OMSetDepthStencilState(nullptr, 0);

		float blendFactor[4] = { 0, 0, 0, 0 };
		context->OMSetBlendState(accumulateBlendState.get(), blendFactor, 0xFFFFFFFF);

		auto cb = voxelConeTracingGICB->CB();
		context->VSSetConstantBuffers(0, 1, &cb);

		context->VSSetShader(accumulateVertex.get(), nullptr, 0);
		context->PSSetShader(accumulatePixel.get(), nullptr, 0);

		context->IASetInputLayout(accumulateInputLayout.get());

		UINT stride = sizeof(Voxel);
		UINT offset = 0;

		context->CopyResource(voxelDrawInstanceBuffer->resource.get(), voxelSamplesBuffer->resource.get());

		const auto& buffer = voxelDrawInstanceBuffer->resource.get();
		context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

		context->DrawInstancedIndirect(voxelIndirectArgsBuffer->resource.get(), 4 * 4);

		rtvs.fill(nullptr);
		context->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), nullptr);

		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		state->EndPerfEvent();
	}

	// Clear buffers
	{
		const auto& samplesUAV = voxelSamplesBuffer->UAV();
		UINT initialCount = 0;
		context->CSSetUnorderedAccessViews(0, 1, &samplesUAV, &initialCount);

		const auto& voxelUAV = voxelBuffer->UAV();
		initialCount = 0;
		context->CSSetUnorderedAccessViews(0, 1, &voxelUAV, &initialCount);
	}

	// Read and average from the accumulated RTVs into the final voxel buffer and the 3d texture
	{
		state->BeginPerfEvent("Readback voxels from RTV to burffer and 3D texture");

		std::array<ID3D11ShaderResourceView*, 3> srvs = {
			voxelAccumulator->SRV(0),
			voxelAccumulator->SRV(1),
			voxelAccumulator->SRV(2)
		};

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			voxelBuffer->UAV()
		};

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		auto cb = voxelConeTracingGICB->CB();
		context->CSSetConstantBuffers(0, 1, &cb);

		context->CSSetShader(postProcessCompute.get(), nullptr, 0);

		float resolutionFloat = static_cast<float>(settings.Resolution);
		uint threadGroups = static_cast<uint>(ceil(resolutionFloat / 8.0f));

		context->Dispatch(threadGroups, threadGroups, threadGroups);

		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		// Generate mip maps
		//context->GenerateMips(lod0Tex->srv.get());

		// Copy voxel count to [1]
		context->CopyStructureCount(voxelIndirectArgsBuffer->resource.get(), 4 * 1, voxelBuffer->UAV());

		state->EndPerfEvent();
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::DrawVoxels()
{
	auto state = globals::state;
	state->BeginPerfEvent("Draw Voxels");

	auto context = globals::d3d::context;

	// Copy voxels to buffer with vertex bind support
	context->CopyResource(voxelDrawInstanceBuffer->resource.get(), voxelBuffer->resource.get());

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

	std::array<ID3D11ShaderResourceView*, 1> srvs = {
		lod0Tex->srv.get()
	};

	if (settings.VoxelDrawMode == VoxelDrawMode::Lighting || settings.VoxelDrawMode == VoxelDrawMode::Final)
		context->PSSetShaderResources(0, (uint)srvs.size(), srvs.data());

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

	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

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

	ID3D11Buffer* buffers[2] = { voxelDrawVertexBuffer->resource.get(), voxelDrawInstanceBuffer->resource.get() };

	context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
	//context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context->DrawInstancedIndirect(voxelIndirectArgsBuffer->resource.get(), 0);

	/*uint lod0Res = GetResolution();
	uint instanceCount = lod0Res * lod0Res * lod0Res;

	context->DrawInstanced(36, instanceCount, 0, 0);*/

	/*context->VSSetShader(prevVertex, nullptr, 0);
	context->PSSetShader(prevPixel, nullptr, 0);

	if (prevVertex)
		prevVertex->Release();

	if (prevPixel)
		prevPixel->Release();*/

	srvs.fill(nullptr);

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i])
			prevRTVs[i]->Release();
	}

	if (prevDSV)
		prevDSV->Release();

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
	state->BeginPerfEvent("Inject Lighting");

	auto context = globals::d3d::context;

	lights.clear();
	lights.reserve(MAX_LIGHTS);

	auto accumulator = *globals::game::currentAccumulator.get();

	bool interior = Util::IsInterior();

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


	// Prepare indirect arguments buffer (ThreadGroupCountX = voxelCount/64)
	{
		state->BeginPerfEvent("Prepare indirect");

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			voxelIndirectArgsBuffer->uav.get()
		};

		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(postProcessIndirectArgsCompute.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);

		state->EndPerfEvent();
	}

	{
		float size = static_cast<float>(settings.Size) / Util::Units::GAME_UNIT_TO_M;
		float3 sizeF3 = float3(size, size, size);

		float resolutionFloat = static_cast<float>(settings.Resolution);

		injectLightingCBData.Min = center - (sizeF3 * 0.5);
		injectLightingCBData.Size = size;
		injectLightingCBData.ResInv = 1.0f / resolutionFloat;
		injectLightingCBData.VoxelSize = size / resolutionFloat;
		injectLightingCBData.LightCount = std::min((uint)lights.size(), MAX_LIGHTS);

		injectLightingCB->Update(injectLightingCBData);

		auto cb = injectLightingCB->CB();

		std::array<ID3D11ShaderResourceView*, 2> srvs = {
			lightBuffer->SRV(),
			voxelBuffer->SRV()
		};

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			lod0Tex->uav.get()
		};

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetConstantBuffers(0, 1, &cb);

		context->CSSetShader(injectLightCompute.get(), nullptr, 0);
		context->DispatchIndirect(voxelIndirectArgsBuffer->resource.get(), 4 * 8);

		context->GenerateMips(lod0Tex->srv.get());

		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Prepass()
{
	if (!settings.EnableVoxelConeTracingGI)
		return;

	auto state = globals::state;
	state->BeginPerfEvent("VoxelConeTracingGI");

	const auto min = float3(-118.078f, 580.67932f, 60.90164f);
	const auto max = float3(421.93579f, 1137.04248f, 252.90166f);

	center = (min + max) * 0.5f;

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
	CompileShaders();
}


uint32_t VoxelConeTracingGI::GetDescriptor(const RE::BSShader::Type& shaderType)
{
	uint32_t descriptor = 0;

	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC);
		break;
	case RE::BSShader::Type::Utility:
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Texture);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Normals);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Vc);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Skinned);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::TreeAnim);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::LodLandscape);
		break;
	default:
		break;
	}	

	return descriptor;
}

template <typename T>
std::vector<std::pair<T, T>> VoxelConeTracingGI::GetShaderDefines(const RE::BSShader::Type& type, const uint32_t& descriptor)
{	
	std::vector<std::pair<T, T>> defines;

	switch (type) {
	case RE::BSShader::Type::Lighting:
		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned))
			defines.emplace_back("SKINNED", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals))
			defines.emplace_back("MODELSPACENORMALS", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC))
			defines.emplace_back("VC", "");

		break;
	case RE::BSShader::Type::Utility:
		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Texture))
			defines.emplace_back("TEXTURE", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Normals))
			defines.emplace_back("NORMALS", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Vc))
			defines.emplace_back("VC", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::Skinned))
			defines.emplace_back("SKINNED", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::TreeAnim))
			defines.emplace_back("TREE_ANIM", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::LodLandscape))
			defines.emplace_back("LOD_LANDSCAPE", "");
		break;
	default:
		break;
	}

	return defines;
}

/*template <typename Inserter>
void CollectShaderDefines(std::span<D3D_SHADER_MACRO> defines, Inserter inserter)
{
	for (const auto& define : defines) {
		if (define.Name == nullptr)
			break;

		inserter(define.Name, define.Definition == nullptr ? "" : define.Definition);
	}
}*/

template <typename T>
void VoxelConeTracingGI::CompileShader(winrt::com_ptr<T>* programPtr, const wchar_t* path, const char* programType, std::vector<std::pair<const char*, const char*>> defines, const std::vector<D3D11_INPUT_ELEMENT_DESC>& inputDesc, winrt::com_ptr<ID3D11InputLayout>* inputLayoutPtr)
{
	if (auto rawPtr = reinterpret_cast<T*>(Util::CompileShader(path, defines, programType, "main", inputDesc, inputLayoutPtr ? inputLayoutPtr->put() : nullptr)))
		programPtr->attach(rawPtr);
}

void VoxelConeTracingGI::CompileShaders()
{
	auto folderPath = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI");

	auto voxelizationFolderPath = folderPath / "Voxelize";


	uint32_t lightingDescriptor = GetDescriptor(RE::BSShader::Type::Lighting);
	const auto lightingDefines = GetShaderDefines<const char*>(RE::BSShader::Type::Lighting, lightingDescriptor);

	for (const auto& define : lightingDefines) {
		logger::info("Lighting Define: ('{}' : '{}')", define.first, define.second);
	}

	voxelizeVertex.emplace(RE::BSShader::Type::Lighting, ShaderUtils::ShaderPermutations<ID3D11VertexShader>(voxelizationFolderPath / "VoxelizeVS.Lighting.hlsl", lightingDefines));
	voxelizePixel.emplace(RE::BSShader::Type::Lighting, ShaderUtils::ShaderPermutations<ID3D11PixelShader>(voxelizationFolderPath / "VoxelizePS.hlsl", lightingDefines));

	/*uint32_t utilityDescriptor = GetDescriptor(RE::BSShader::Type::Utility);
	const auto utilityDefines = GetShaderDefines<const char*>(RE::BSShader::Type::Utility, utilityDescriptor);

	for (const auto& define : utilityDefines) {
		logger::info("Utility Define: ('{}' : '{}')", define.first, define.second);
	}

	voxelizeVertex.emplace(RE::BSShader::Type::Utility, ShaderUtils::ShaderPermutations<ID3D11VertexShader>(voxelizationFolderPath / "VoxelizeVS.Utility.hlsl", utilityDefines));
	voxelizePixel.emplace(RE::BSShader::Type::Utility, ShaderUtils::ShaderPermutations<ID3D11PixelShader>(voxelizationFolderPath / "VoxelizePS.hlsl", utilityDefines));*/

	CompileShader(&voxelizeGeometry, (voxelizationFolderPath / "VoxelizeGS.hlsl").c_str(), "gs_5_0");

	auto accumulateFolderPath = folderPath / "Accumulate";
	CompileShader(&accumulateVertex, (accumulateFolderPath / "AccumulateVS.hlsl").c_str(), "vs_5_0", {}, accumulateInputDesc, &accumulateInputLayout);
	CompileShader(&accumulatePixel, (accumulateFolderPath / "AccumulatePS.hlsl").c_str(), "ps_5_0");

	auto debugFolderpath = folderPath / "Debug";
	CompileShader(&drawVertex, (debugFolderpath / "DrawVS.hlsl").c_str(), "vs_5_0", {}, voxelInputDesc, &voxelDrawInputLayout);

	for (size_t i = 0; i < drawPixelVariants.size(); i++) {
		CompileShader(&drawPixelVariants[i], (debugFolderpath / "DrawPS.hlsl").c_str(), "ps_5_0", { { "DRAW_MODE", std::to_string(i).c_str() } });
	}
	
	CompileShader(&postProcessIndirectArgsCompute, (folderPath / "PostProcessIndirectArgsCS.hlsl").c_str(), "cs_5_0");
	CompileShader(&postProcessCompute, (folderPath / "PostProcessCS.hlsl").c_str(), "cs_5_0");
	CompileShader(&voxelArgsCompute, (folderPath / "VoxelArgsCS.hlsl").c_str(), "cs_5_0");
	CompileShader(&injectLightCompute, (folderPath / "InjectLightingCS.hlsl").c_str(), "cs_5_0");
	CompileShader(&debugCompute, (folderPath / "DebugCS.hlsl").c_str(), "cs_5_0");
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