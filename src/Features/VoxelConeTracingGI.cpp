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

#include "Deferred.h"
#include "Utils/UI.h"
#include "Utils/Format.h"
#include "ShaderCache.h"

using namespace std::chrono;

static constexpr uint MAX_LIGHTS = 1024;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VoxelConeTracingGI::Settings,
	Enabled,
	Resolution,
	Size,
	ClipmapCount,
	ScaleFactor,
	UpdateRate,
	ForceUpdate,
	Interior,
	Exterior,
	DebugDrawMode)

////////////////////////////////////////////////////////////////////////////////////

void VoxelConeTracingGI::RestoreDefaultSettings()
{
	settings = {};

	changedSettings = ChangedSetting::VolumeResolution | ChangedSetting::ConeCount;
}

void VoxelConeTracingGI::LoadSettings(json& o_json)
{
	settings = o_json;

	changedSettings = ChangedSetting::VolumeResolution | ChangedSetting::ConeCount;
}

void VoxelConeTracingGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VoxelConeTracingGI::DrawGeneralSettings()
{
	if (ImGui::CollapsingHeader("General settings")) {
		// Resolution
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
				changedSettings |= ChangedSetting::VolumeResolution;
			}

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Volume resolution per axis\n");
			}
		}

		// Size
		ImGui::InputInt("Size", &settings.Size);
		settings.Size = std::max(1, settings.Size);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("World size of first volume\n");
		}

		// Clipmap ammount
		ImGui::SliderInt("Clipmaps", &settings.ClipmapCount, 1, 8);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Number of clipmaps to use\n");
		}

		// Clipmap scale factor
		{
			ImGui::SliderInt("Clipmap Scale Factor", &settings.ScaleFactor, 2, 10);

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("The world space size multiplier for each subsequent clipmap\n");
			}

			const auto& clipmapCount = settings.ClipmapCount;

			std::string label;

			for (int i = 0; i < clipmapCount; ++i) {
				label += std::format("Clip{} size: {}", i, settings.Size * std::pow(settings.ScaleFactor, i));

				if (i < clipmapCount - 1) {
					label += ", ";
				}
			}

			ImGui::Text(label.c_str());
		}

		// Update rate
		{
			int updateRate = static_cast<int>(settings.UpdateRate);

			ImGui::SliderInt("Voxelization rate", &updateRate, 1, 8);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How often to update voxelization. 1 - Every frame, 2 - Every other frame, 3 - Every third frame, etc...");
			}
			settings.UpdateRate = static_cast<uint8_t>(updateRate);
		}

		// Force update all voxels
		{
			bool forceUpdate = settings.ForceUpdate;
			ImGui::Checkbox("Force Update All Voxels", &forceUpdate);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Enables eager voxelization, by default the world is scanned for changes and rendered efficiently");
			}
			settings.ForceUpdate = forceUpdate;
		}
	}
}

void VoxelConeTracingGI::DrawConeSettings(ConeSettings& coneSettings, bool diffuse)
{
	const char* coneName = diffuse ? "Diffuse" : "Specular";

	if (ImGui::BeginTabItem(coneName)) {
	//if (ImGui::TreeNodeEx(coneName, ImGuiTreeNodeFlags_DefaultOpen, std::format("{}", coneName).c_str())) {
		if (ImGui::TreeNodeEx(std::format("{}_Step", coneName).c_str(), ImGuiTreeNodeFlags_DefaultOpen, "Step")) {
			auto& stepSettings = coneSettings.Step;

			if (ImGui::SliderFloat("Length", &stepSettings.Length, 0.25f, 2.0f)) {
				stepSettings.Length = std::max(stepSettings.Length, 0.25f);
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Step length scale.\nLow value = improved accuracy/quality - slow. High value: lower quality/accuracy - faster.");

			ImGui::SliderFloat("Radius", &stepSettings.Radius, 0.25f, 2.0f);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Step radius scale.");

			ImGui::SliderFloat("MipScale", &stepSettings.MipScale, 0.0f, 1.0f);
			/*if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Step light accumulation scale.");*/

			ImGui::SliderFloat("Alpha", &stepSettings.Alpha, 0.0f, 10.0f);
			
			ImGui::TreePop();
		}

		if (diffuse) {
			if (ImGui::SliderInt("Count", &coneSettings.Count, 5, 32)) {
				changedSettings |= ChangedSetting::ConeCount;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Cones to trace per pixel.\nHow many cones we will shoot from each pixel in the screen.");
		} else {
			ImGui::SliderInt("Min. Angle", &coneSettings.MinAngle, 0, 90);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Starting cone angle.");
		}

		ImGui::SliderInt("Angle", &coneSettings.Angle, 0, 90);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Cone angle.");

		ImGui::SliderInt("Distance", &coneSettings.Distance, 1, 1000);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Maximum cone distance.\nA cone that goes further collects more light.");

		ImGui::SliderFloat("Alpha", &coneSettings.Alpha, 0.1f, 2.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Alpha is visibility, it is used to control cone length and stop bleeding.\n");

		ImGui::SliderFloat("Offset", &coneSettings.Offset, 0.0f, 10.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Start position offset to avoid self-lighting.");

		ImGui::SliderFloat("Strength", &coneSettings.Strength, 0.0f, 10.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Final accumulated color strength.");

		//ImGui::TreePop();
		ImGui::EndTabItem();
	}
}

void VoxelConeTracingGI::DrawEnvironmentSettings(EnvironmentSettings& environmentSettings, const char* environmentName)
{
	if (ImGui::CollapsingHeader(environmentName)) {
		//if (ImGui::TreeNodeEx(std::format("{}_Lighting", environmentName).c_str(), ImGuiTreeNodeFlags_DefaultOpen, "Lighting strength")) 
		ImGui::Text("Lighting");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Controls the strength of injected lighting to the volume.");

		{
			auto& lighting = environmentSettings.Lighting;

			ImGui::SliderFloat("Direct", &lighting.Direct, 0.0f, 10.0f);
			ImGui::SliderFloat("Directional light/Sun", &lighting.Directional, 0.0f, 10.0f);
			ImGui::SliderFloat("Omni", &lighting.Omni, 0.0f, 10.0f);
			ImGui::SliderFloat("Emissive", &lighting.Emissive, 0.0f, 10.0f);
			ImGui::SliderFloat("Ambient", &lighting.Ambient, 0.0f, 10.0f);

			//ImGui::TreePop();
		}
		/*if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Controls the strength of injected light into the volume");*/

		//if (ImGui::TreeNodeEx(std::format("{}_Tracing", environmentName).c_str(), ImGuiTreeNodeFlags_DefaultOpen, "Tracing")) 
		ImGui::Text("Tracing");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("To simulate a cone, the radius increases as we step through the volume collecting lighting along the way.");
			ImGui::Text("Use these settings to fine tune global illumination.");
		}

		{
			auto& tracing = environmentSettings.Tracing;

			if (ImGui::BeginTabBar("ConeSettings")) {
				//DrawConeSettings(tracing.General, "General tracing");
				DrawConeSettings(tracing.Diffuse, true);
				DrawConeSettings(tracing.Specular, false);

				ImGui::EndTabBar();		
			}

			//ImGui::Text("Material settings");
			//ImGui::SliderFloat("Specular Strength", &tracing.SpecularStrength, 0.0f, 5.0f);
			//ImGui::SliderFloat("Roughness Scale", &tracing.RoughnessScale, 0.0f, 5.0f);

			if (ImGui::BeginTable("Resolution mode", 3)) {
				int resolutionMode = static_cast<int>(tracing.ResolutionMode);

				ImGui::TableNextColumn();
				ImGui::RadioButton("Full Res", &resolutionMode, 0);
				ImGui::TableNextColumn();
				ImGui::RadioButton("Half Res", &resolutionMode, 1);
				ImGui::TableNextColumn();
				ImGui::RadioButton("Quarter Res", &resolutionMode, 2);

				tracing.ResolutionMode = static_cast<uint8_t>(resolutionMode);

				ImGui::EndTable();
			}

			int additionalBounces = static_cast<int>(tracing.AdditionalBounces);
			ImGui::SliderInt("Additional Bounces", &additionalBounces, 0, 8);
			tracing.AdditionalBounces = static_cast<uint8_t>(additionalBounces);

			//ImGui::TreePop();
		}

		/*if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Cone tracing settings");*/	
	}
}

void VoxelConeTracingGI::DrawSettings()
{
	bool enabled = settings.Enabled;
	ImGui::Checkbox("Enable Voxel Cone Tracing GI", &enabled);
	settings.Enabled = enabled;

    if (!settings.Enabled)
		ImGui::BeginDisabled(true);

	{
		DrawGeneralSettings();

		DrawEnvironmentSettings(settings.Interior, "Interior");
		DrawEnvironmentSettings(settings.Exterior, "Exterior");

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Queue Size: {}", queuedTriShapes.size()).c_str());

		if (ImGui::TreeNodeEx("VoxelizationHistory", ImGuiTreeNodeFlags_DefaultOpen, std::format("History ({})", history.size()).c_str())) {
			const auto num_elements = eastl::min<eastl_size_t>(5, history.size());

			if (num_elements > 0) {
				ImGui::Text(std::format("Last {} voxelization passes:", num_elements).c_str());

				if (ImGui::BeginTable("VoxelizationHistoryTable", 2, ImGuiTableFlags_Borders)) {
					const auto start_iterator = history.end() - std::min(num_elements, history.size());
					const std::vector<History> last_five_elements(start_iterator, history.end());

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

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Debug", ImGuiTreeNodeFlags_DefaultOpen)) {
		std::array<std::string, DebugDrawMode::Count> items;

		const auto& itemsView = magic_enum::enum_names<DebugDrawMode>();
		for (size_t i = 0; i < items.size(); i++) {
			items[i] = std::string(itemsView[i]);
		}

		if (ImGui::BeginCombo("Display Mode", items[settings.DebugDrawMode].c_str())) {
			for (size_t i = 0; i < items.size(); i++) {
				bool isSelected = (settings.DebugDrawMode == i);

				if (ImGui::Selectable(items[i].c_str(), isSelected))
					settings.DebugDrawMode = static_cast<DebugDrawMode>(i);

				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}

		ImGui::TreePop();
	}

	if (!settings.Enabled)
		ImGui::EndDisabled(); 
}

void VoxelConeTracingGI::SetupScreenTextures()
{
	logger::debug("Creating screen textures...");
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
				.Quality = 0 },
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
				.MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MipSlice = 0 }
		};

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MipSlice = 0 }
		};

		diffuseGI = eastl::make_unique<Texture2D>(texDesc);
		diffuseGI->CreateSRV(srvDesc);
		diffuseGI->CreateUAV(uavDesc);
		diffuseGI->CreateRTV(rtvDesc);

		specularGI = eastl::make_unique<Texture2D>(texDesc);
		specularGI->CreateSRV(srvDesc);
		specularGI->CreateUAV(uavDesc);
		specularGI->CreateRTV(rtvDesc);
	}
}

void VoxelConeTracingGI::SetupVolumeTextures()
{
	uint resolution = settings.Resolution;

	logger::debug("Creating volume textures...");
	{
		D3D11_TEXTURE3D_DESC volumeTexDesc{
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

		D3D11_SHADER_RESOURCE_VIEW_DESC volumeSRVDesc = {
			.Format = volumeTexDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MostDetailedMip = 0,
				.MipLevels = ~0u }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC volumeUAVDesc = {
			.Format = volumeTexDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = ~0u }
		};

		clipmapVolumes.clear();

		for(int i=0; i < settings.ClipmapCount; i++) {
			auto clipmap = eastl::make_unique<Texture3D>(volumeTexDesc);

			clipmap->CreateSRV(volumeSRVDesc);
			clipmap->CreateUAV(volumeUAVDesc);

			clipmapVolumes.push_back(eastl::move(clipmap));
		}
	}

	logger::debug("Creating render targets...");
	{
		//int slicesPerRow = std::max(1.0f, 16384.0f / resolution);
		//int rows = (resolution + slicesPerRow - 1) / slicesPerRow

		//size.width = slicesPerRow * resolution;
		//size.height = rows * resolution;

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = resolution * resolution, // This will break for 256 and up but...
			.Height = resolution,
			.MipLevels = 1,
			.ArraySize = 3,  // Albedo, Emission and Normal
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, // D3D11_BIND_UNORDERED_ACCESS
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
				.ArraySize = 1 }
		};

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = {
				.MipSlice = 0,
				.FirstArraySlice = 0,
				.ArraySize = 1 }
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
}

void VoxelConeTracingGI::SetupBuffers()
{
	uint resolution = settings.Resolution;
	uint maxVoxelCount = resolution * resolution * resolution;
	
	// Voxel Samples Buffer
	{
		auto samplesBufferDesc = StructuredBufferDesc<Voxel>(maxVoxelCount, true, false);

		clipmapSamplesBuffer.clear();

		for (int i = 0; i < settings.ClipmapCount; i++) {
			auto samplesBuffer = eastl::make_unique<StructuredBuffer>(samplesBufferDesc, maxVoxelCount);
			samplesBuffer->CreateUAV(true);
			samplesBuffer->CreateSRV();

			clipmapSamplesBuffer.push_back(eastl::move(samplesBuffer));
		}
	}

	// Voxel Buffer
	{
		auto voxelBufferDesc = StructuredBufferDesc<Voxel>(maxVoxelCount, true, false);
		voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, maxVoxelCount);
		voxelBuffer->CreateUAV(true);
		voxelBuffer->CreateSRV();
	}

	// Voxel Draw Instance Buffer
	{
		D3D11_BUFFER_DESC voxelDrawBufferDesc = {
			.ByteWidth = sizeof(Voxel) * maxVoxelCount,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC voxelDrawInstanceUAVDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = voxelDrawBufferDesc.ByteWidth,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW 
			}
		};

		voxelDrawInstanceBuffer = eastl::make_unique<Buffer>(voxelDrawBufferDesc);
	}
}

void VoxelConeTracingGI::SetupViewport()
{
	float resolutionFloat = static_cast<float>(settings.Resolution);

	//float viewportResolution = resolutionFloat * std::pow(settings.ScaleFactor, settings.ClipmapCount - 1);

	voxelizeViewport.TopLeftX = 0.0f;
	voxelizeViewport.TopLeftY = 0.0f;
	voxelizeViewport.Width = resolutionFloat;
	voxelizeViewport.Height = resolutionFloat;
	voxelizeViewport.MinDepth = 0.0f;
	voxelizeViewport.MaxDepth = 1.0f;

	accumulateViewport.TopLeftX = 0.0f;
	accumulateViewport.TopLeftY = 0.0f;
	accumulateViewport.Width = resolutionFloat * resolutionFloat;  // Breaky breaky
	accumulateViewport.Height = resolutionFloat;
	accumulateViewport.MinDepth = 0.0f;
	accumulateViewport.MaxDepth = 1.0f;
}


void VoxelConeTracingGI::SetupResources()
{
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->QueryInterface(__uuidof(ID3D11Device3), device3.put_void()));

	logger::debug("Creating Constant buffers...");
	{
		voxelConeTracingGICB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VoxelConeTracingGICB>());	
		injectLightingCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<InjectLightingCB>());	
		vsFrameBufferPSCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VSFrameBufferPSCB>());	

		coneTraceCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<TraceSettings>());
		screenTraceCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<ScreenTraceCB>());		
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

		// Light buffer
		auto lightBufferDesc = StructuredBufferDesc<Light>(MAX_LIGHTS, false, true);
		lightBuffer = eastl::make_unique<StructuredBuffer>(lightBufferDesc, MAX_LIGHTS);
		lightBuffer->CreateSRV();
	}

	logger::debug("Creating Raw buffers...");
	{
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
		DX::ThrowIfFailed(device->CreateSamplerState(&lod0SamplerDesc, volumeSampler.put()));
	}

	// Raster State
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
		DX::ThrowIfFailed(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)));
		logger::warn("CheckFeatureSupport: ConservativeRasterizationTier: {}", magic_enum::enum_name(options2.ConservativeRasterizationTier));

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

	// Depth Stencil State
	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, voxelDrawDSS.put()));
	}

	// Setup
	SetupVolumeTextures();
	SetupScreenTextures();
	SetupBuffers();
	SetupViewport();

	CompileShaders();
}

std::vector<float4> VoxelConeTracingGI::GenerateFibonacciCosineHemisphereSamples(int n)
{
	std::vector<float4> samples;
	samples.reserve(n);

	const double GOLDEN_ANGLE = 2.0 * std::numbers::pi * (1.0 - 1.0 / std::numbers::phi);

	for (int i = 0; i < n; ++i) {
		// Uniform value from [0, 1)
		float u = (i + 0.5f) / static_cast<float>(n);

		// Cosine-weighted mapping using inverse CDF
		float z = std::sqrt(1.0f - u);  // Cosine-weighted
		float r = std::sqrt(u);         // Projected radius on the disk

		float theta = static_cast<float>(GOLDEN_ANGLE * i);

		float x = r * std::cos(theta);
		float y = r * std::sin(theta);

		samples.push_back({ x, y, z, 1.0f });  // In tangent space
	}

	return samples;
}

bool IntersectAABB(const float3& boxAMin, const float3& boxAMax, const float3& boxBMin, const float3& boxBMax)
{
	return (boxAMax.x >= boxBMin.x && boxAMin.x <= boxBMax.x) &&
	       (boxAMax.y >= boxBMin.y && boxAMin.y <= boxBMax.y) &&
	       (boxAMax.z >= boxBMin.z && boxAMin.z <= boxBMax.z);
}

bool ContainedAABB(const float3& boxAMin, const float3& boxAMax, const float3& boxBMin, const float3& boxBMax)
{
	return (boxAMin.x >= boxBMin.x && boxAMax.x <= boxBMax.x) &&
	       (boxAMin.y >= boxBMin.y && boxAMax.y <= boxBMax.y) &&
	       (boxAMin.z >= boxBMin.z && boxAMax.z <= boxBMax.z);
}

void VoxelConeTracingGI::BSShader_SetupGeometry(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	const auto shaderType = This->shaderType.get();

	if (loaded && Enabled()) {
		if (renderingWorld) {
			const auto geometry = Pass->geometry;
			const auto triShape = geometry->AsTriShape();

			if (triShape == nullptr) {
				/*logger::warn(
					"BSShader_SetupGeometry - nullptr triShape for Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}",
					reinterpret_cast<uintptr_t>(Pass),
					reinterpret_cast<uintptr_t>(This),
					magic_enum::enum_name(shaderType),
					reinterpret_cast<uintptr_t>(geometry),
					geometry->name.c_str());*/
			} else {
				auto it = cachedTriShapes.find(triShape);

				auto pixelDescriptor = globals::state->currentPixelDescriptor;

				bool skipTriShape = false;

				/*if (geometry->name == "Farmhouse05:6") {
					std::string defs;

					constexpr auto entries = magic_enum::enum_entries<SIE::ShaderCache::LightingShaderFlags>();
					for (const auto& flag : entries) {
						if (pixelDescriptor & static_cast<uint32_t>(flag.first))
							defs += std::format("( {}, {} ) ", flag.second, static_cast<uint32_t>(flag.first));
					};

					logger::info(
						"BSShader_SetupGeometry - Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}, Defines: {}",
						reinterpret_cast<uintptr_t>(Pass),
						reinterpret_cast<uintptr_t>(This),
						magic_enum::enum_name(shaderType),
						reinterpret_cast<uintptr_t>(geometry),
						geometry->name.c_str(),
						defs);				
				}*/

				/*if (shaderType == RE::BSShader::Type::Effect) {
					std::string defs;

					constexpr auto entries = magic_enum::enum_entries<SIE::ShaderCache::EffectShaderFlags>(); 
					for (const auto& flag : entries) {
						if (pixelDescriptor & static_cast<uint32_t>(flag.first)) 
							defs += std::format("( {}, {} ) ", flag.second, static_cast<uint32_t>(flag.first));
					};

					logger::info(
						"BSShader_SetupGeometry - Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}, Defines: {}",
						reinterpret_cast<uintptr_t>(Pass),
						reinterpret_cast<uintptr_t>(This),
						magic_enum::enum_name(shaderType),
						reinterpret_cast<uintptr_t>(geometry),
						geometry->name.c_str(),
						defs);
				}*/

				if (shaderType == RE::BSShader::Type::Effect) {
					if (pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::AddBlend) ||
						pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::MultBlend) ||
						pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Particles) || 
						pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::StripParticles) || 
						pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::AlphaTest) || 
						pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::SkyObject)) {
						skipTriShape = true;
					}
				}

				if (geometry->worldBound.radius == 0) {
					skipTriShape = true;
				}

				const auto& transform = geometry->world;

				const auto& modelData = triShape->GetModelData().modelBound;

				const auto& modelCenterTransformed = transform * modelData.center;
				const auto& modelCenter = float3(modelCenterTransformed.x, modelCenterTransformed.y, modelCenterTransformed.z);

				const auto& r = float3(modelData.radius, modelData.radius, modelData.radius);

				const auto& aabbMin = modelCenter - r;
				const auto& aabbMax = modelCenter + r;

				const auto& lastClipmap = voxelConeTracingGICBData.Clipmaps[voxelConeTracingGICBData.ClipmapCount-1];

				if (!IntersectAABB(aabbMin, aabbMax, lastClipmap.Min, lastClipmap.Max)) {
					skipTriShape = true;
				}

				if (!skipTriShape) {
					// TriShape not cached, lets add to cache and queue
					if (it == cachedTriShapes.end()) {
						cachedTriShapes.emplace(triShape);
						queuedTriShapes.emplace(triShape);
					} else if (settings.ForceUpdate == TRUE) {
						queuedTriShapes.emplace(triShape);
					}

					if (unCulledGeometry.find(geometry) != unCulledGeometry.end())
						return;
				}
			}
		}
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] BSShader::SetupGeometry");

	if (shaderType == RE::BSShader::Type::Lighting)
		Hooks::BSShader_SetupGeometry<RE::BSShader::Type::Lighting>::func(This, Pass, RenderFlags);
	else if (shaderType == RE::BSShader::Type::Effect)
		Hooks::BSShader_SetupGeometry<RE::BSShader::Type::Effect>::func(This, Pass, RenderFlags);

}

void VoxelConeTracingGI::BSShader_RestoreGeometry(RE::BSShader* This, RE::BSRenderPass* Pass)
{
	if (!renderingWorld)
		return;

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();

	if (triShape == nullptr)
		return;

	auto it = queuedTriShapes.find(triShape);

	// TriShape not queued
	if (it == queuedTriShapes.end())
		return;

	auto it2 = cachedTriShapes.find(triShape);

	// TriShape not cached
	if (it2 == cachedTriShapes.end())
		return;

	const auto shaderType = This->shaderType.get();

	// Vertex Shader
	auto vs = voxelizeVertex.find(shaderType);

	if (vs == voxelizeVertex.end())
		return;

	auto vertexShaderPermutations = vs->second;

	auto state = globals::state;

	auto vertexDescriptor = state->currentVertexDescriptor;

	std::map<std::string, std::string> vertexDefines;
	for (const auto& item: GetShaderDefines<std::string>(shaderType, vertexDescriptor)) {
		vertexDefines.emplace(item.first, item.second);
	}

	auto vertexShader = vertexShaderPermutations.Get(vertexDefines);
	
	if (vertexShader == nullptr) {
		std::string defs;

		for (const auto& item : vertexDefines) {
			defs += std::format("(\"{}\": \"{}\")", item.first, item.second);
		};

		logger::info(
			"BSShader_RestoreGeometry - vertex shader not found for Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}, Defines: {}",
			reinterpret_cast<uintptr_t>(Pass),
			reinterpret_cast<uintptr_t>(This),
			magic_enum::enum_name(shaderType),
			reinterpret_cast<uintptr_t>(geometry),
			geometry->name.c_str(),
			defs.c_str());
			
		return;
	}

	// Vertex Shader
	auto ps = voxelizePixel.find(shaderType);

	if (ps == voxelizePixel.end())
		return;

	auto pixelShaderPermutations = ps->second;

	auto pixelDescriptor = state->currentPixelDescriptor;

	std::map<std::string, std::string> pixelDefines;

	// Only Effect pixel shader has defines
	if (shaderType == RE::BSShader::Type::Effect) {
		for (const auto& item: GetShaderDefines<std::string>(shaderType, pixelDescriptor)) {
			pixelDefines.emplace(item.first, item.second);
		}
	}

	auto pixelShader = pixelShaderPermutations.Get(pixelDefines);

	if (pixelShader == nullptr) {
		std::string defs;

		for (const auto& item : pixelDefines) {
			defs += std::format("(\"{}\": \"{}\")", item.first, item.second);
		}

		logger::info(
			"BSShader_RestoreGeometry - pixel shader not found for Pass: [0x{:x}], Shader [0x{:x}]: {}, Geometry [0x{:x}]: {}, Defines: {}",
			reinterpret_cast<uintptr_t>(Pass),
			reinterpret_cast<uintptr_t>(This),
			magic_enum::enum_name(shaderType),
			reinterpret_cast<uintptr_t>(geometry),
			geometry->name.c_str(),
			defs.c_str());

		return;
	}

	const auto triShapeRuntime = triShape->GetTrishapeRuntimeData();

	// Render here
	auto context = globals::d3d::context;

	state->BeginPerfEvent(unCulledGeometry.find(geometry) == unCulledGeometry.end()  ? "[VCTGI] Voxelization" : "[VCTGI] Voxelization [UnCulled]");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] Voxelization");

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

	context->RSSetViewports(1, &voxelizeViewport);

	/*float blendFactor[4] = { 0, 0, 0, 0 };
	context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);*/

	// Clipmap handling
	const auto& transform = geometry->world;

	const auto& modelData = triShape->GetModelData().modelBound;

	const auto& modelCenterTransformed = transform * modelData.center;
	const auto& modelCenter = float3(modelCenterTransformed.x, modelCenterTransformed.y, modelCenterTransformed.z);

	const auto& r = float3(modelData.radius, modelData.radius, modelData.radius);

	const auto& aabbMin = modelCenter - r;
	const auto& aabbMax = modelCenter + r;

	std::vector<uint> renderClipmap;
	renderClipmap.reserve(settings.ClipmapCount);

	// Samples UAV
	std::array<ID3D11UnorderedAccessView*, 8> uavs = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

	for (uint i = 0; i < voxelConeTracingGICBData.ClipmapCount; i++)
	{
		const auto& clipmap = voxelConeTracingGICBData.Clipmaps[i];

		bool contains = ContainedAABB(aabbMin, aabbMax, clipmap.Min, clipmap.Max);
		bool intersects = IntersectAABB(aabbMin, aabbMax, clipmap.Min, clipmap.Max);

		if (contains || intersects) {
			renderClipmap.push_back(i);
			uavs[i] = clipmapSamplesBuffer[i]->UAV();

			// Model is contained completely inside and doesn't intersect any other clipmap 
			if (contains) {
				break;
			}
		}
	}

	// Render
	uint indexCount = triShapeRuntime.triangleCount * 3;
	//context->DrawIndexed(indexCount, 0, 0);

	uint instanceCount = (uint)renderClipmap.size();
	uint firstInstance = renderClipmap.front();

	// Set samples UAV
	context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, (uint)uavs.size(), uavs.data(), nullptr);

	// Render
	context->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, firstInstance);

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

		if (((changedSettings & ChangedSetting::VolumeResolution) != ChangedSetting::None) || ((changedSettings & ChangedSetting::ClipmapCount) != ChangedSetting::None)) {
			SetupVolumeTextures();
			SetupBuffers();
			SetupViewport();

			changedSettings &= ~ChangedSetting::VolumeResolution;
			changedSettings &= ~ChangedSetting::ClipmapCount;
		}

		if ((changedSettings & ChangedSetting::ScreenResolution) != ChangedSetting::None) {
			SetupScreenTextures();

			changedSettings &= ~ChangedSetting::ScreenResolution;
		}
		
		vsFrameBufferPSData.CameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
		vsFrameBufferPSCB->Update(vsFrameBufferPSData);
		auto vsFBPSCB = vsFrameBufferPSCB->CB();

		float size = static_cast<float>(settings.Size) / Util::Units::GAME_UNIT_TO_M;

		uint resolution = settings.Resolution;
		float resolutionFloat = static_cast<float>(resolution);

		float voxelSize = size / resolutionFloat;

		const auto& avgEyePosition = Util::GetAverageEyePosition();

		const auto& avgEyePositionSnapped = float3(
			floor(avgEyePosition.x / voxelSize) * voxelSize,
			floor(avgEyePosition.y / voxelSize) * voxelSize,
			floor(avgEyePosition.z / voxelSize) * voxelSize);

		center = avgEyePositionSnapped + float3(voxelSize * 0.5f, voxelSize * 0.5f, voxelSize * 0.5f);
		//center = avgEyePositionSnapped;

		/*const auto min = float3(-118.078f, 580.67932f, 60.90164f);
		const auto max = float3(421.93579f, 1137.04248f, 252.90166f);

		center = (min + max) * 0.5f;*/

		voxelConeTracingGICBData.Res = resolution;
		voxelConeTracingGICBData.ResRcp = 10000.0f / resolutionFloat;

		voxelConeTracingGICBData.ClipmapCount = static_cast<uint>(settings.ClipmapCount);
		
		for (int i = 0; i < settings.ClipmapCount; i++)
		{
			float clipSize = static_cast<float>(settings.Size * pow(settings.ScaleFactor, i)) / Util::Units::GAME_UNIT_TO_M;
			float3 clipExents = float3(clipSize, clipSize, clipSize) * 0.5f;

			voxelConeTracingGICBData.Clipmaps[i] = {
				.Min = center - clipExents,
				.Size = clipSize,
				.Max = center + clipExents,
				.SizeRcp = 10000.0f / clipSize
			};
		}

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

			if (history.size() >= MAX_HISTORY) {
				history.pop_front();
			}

			history.push_back(record);
		}

		Main_RenderWorldAfter();
		
		// Let's make sure its at the absolute end of everything
		lastUpdateFrame = globals::state->frameCount;
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

void VoxelConeTracingGI::BSBatchRenderer_RenderPassImmediately(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto state = globals::state;
	state->BeginPerfEvent("BSBatchRenderer::RenderPassImmediately");

	auto& geometry = a_pass->geometry;
	bool unCulled = false;

	if (geometry && geometry->worldBound.radius > 0.0f && geometry->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
		unCulled = true;
		geometry->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
		unCulledGeometry.insert(geometry);
	}

	Hooks::BSBatchRenderer_RenderPassImmediately::func(a_pass, a_technique, a_alphaTest, a_renderFlags);

	if (geometry && unCulled) {
		geometry->GetFlags().set(RE::NiAVObject::Flag::kHidden);
		unCulledGeometry.erase(geometry);
	}

	state->EndPerfEvent();
}

VoxelConeTracingGI::VoxelConeTracingGICB VoxelConeTracingGI::GetCommonBufferData() const
{
	return voxelConeTracingGICBData;
}

void VoxelConeTracingGI::Main_RenderWorldAfter()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] WorldAfter");

	if (settings.DebugDrawMode != DebugDrawMode::None && settings.DebugDrawMode != DebugDrawMode::Count) {
		DebugDrawVoxels();
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::PostProcess()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] PostProcess");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] PostProcess");

	auto context = globals::d3d::context;

	// Write samples to RTVs
	{
		state->BeginPerfEvent("Blend samples to RTV");

		ZoneScoped;
		TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] Blend samples to RTV");

		// Copy sample count to [7]
		context->CopyStructureCount(voxelIndirectArgsBuffer->resource.get(), 4 * 4, clipmapSamplesBuffer->UAV());

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

		context->CopyResource(voxelDrawInstanceBuffer->resource.get(), clipmapSamplesBuffer->resource.get());

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
		ZoneScoped;
		TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] Clear buffers");

		std::array<ID3D11UnorderedAccessView*, 8> uavs = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

		for (uint i = 0; i < voxelConeTracingGICBData.ClipmapCount; i++) {
			uavs[i] = clipmapSamplesBuffer[i]->UAV();
		}

		UINT initialCount = 0;
		context->CSSetUnorderedAccessViews(0, (UINT)uavs.size(), uavs.data(), &initialCount);

		const auto& voxelUAV = voxelBuffer->UAV();
		initialCount = 0;
		context->CSSetUnorderedAccessViews(0, 1, &voxelUAV, &initialCount);
	}

	// Read and average from the accumulated RTVs into the final voxel buffer and the 3d texture
	{
		state->BeginPerfEvent("[VCTGI] Readback from RTV to buffer/3D texture");

		ZoneScoped;
		TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] Readback from RTV to buffer/3D texture");

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

		// Copy voxel count to [1]
		context->CopyStructureCount(voxelIndirectArgsBuffer->resource.get(), 4 * 1, voxelBuffer->UAV());

		state->EndPerfEvent();
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::ScreenConeTrace()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] ScreenConeTrace");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] ScreenConeTrace");

	auto context = globals::d3d::context;

	if ((changedSettings & ChangedSetting::ConeCount) != ChangedSetting::None || vctGIInteriorCompute == nullptr || vctGIExteriorCompute == nullptr) {
		ClearVCTShader();
	}

	const auto& currentEnvSettings = CurrentEnvironmentSettings();

	auto eye = Util::GetCameraData(0);

	float2 ndcToViewMult = float2(2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1));
	float2 ndcToViewAdd = float2(-1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1));

	float2 screenSize = globals::state->screenSize;
	float2 screenSizeDynamic = Util::ConvertToDynamic(screenSize);

	screenTraceCBData.NDCToView = float4(ndcToViewMult.x, ndcToViewMult.y, ndcToViewAdd.x, ndcToViewAdd.y);
	screenTraceCBData.RcpFrameDim = float2(1.0) / screenSizeDynamic;
	screenTraceCBData.MaxMipLevel = log2(static_cast<float>(settings.Resolution));
	const auto& cones = GenerateFibonacciCosineHemisphereSamples(currentEnvSettings.Tracing.Diffuse.Count);
	std::copy(cones.begin(), cones.end(), screenTraceCBData.Cones);

	screenTraceCB->Update(screenTraceCBData);

	/*coneTraceCBata.Diffuse = currentEnvSettings.Tracing.Diffuse;
	coneTraceCBata.Specular = currentEnvSettings.Tracing.Specular;
	coneTraceCBata.ResolutionMode = currentEnvSettings.Tracing.ResolutionMode;
	coneTraceCBata.ResolutionMode = currentEnvSettings.Tracing.AdditionalBounces;*/
	coneTraceCB->Update(currentEnvSettings.Tracing);

	auto renderer = globals::game::renderer;

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];

	std::array<ID3D11ShaderResourceView*, 4> srvs = {
		depth.depthSRV,
		normalRoughness.SRV,
		reflectance.SRV,
		clipmapVolumes[0]->srv.get()
	};

	std::array<ID3D11UnorderedAccessView*, 2> uavs = {
		diffuseGI->uav.get(),
		specularGI->uav.get()
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	/*std::array<ID3D11SamplerState*, 1> samplers = {
		volumeSampler.get()
	};
	context->CSSetSamplers(3, (uint)samplers.size(), samplers.data());*/

	ID3D11SamplerState* sampler = volumeSampler.get();
	context->CSSetSamplers(3, 1, &sampler);

	std::array<ID3D11Buffer*, 3> cbs = {
		voxelConeTracingGICB->CB(),
		screenTraceCB->CB(),
		coneTraceCB->CB()
	};
	context->CSSetConstantBuffers(0, (uint)cbs.size(), cbs.data());

	uint widthThreads = static_cast<uint>(std::ceil(screenSizeDynamic.x / 16.0f));
	uint heightThreads = static_cast<uint>(std::ceil(screenSizeDynamic.y / 8.0f));

	context->CSSetShader((Util::IsInterior() ? vctGIInteriorCompute.get() : vctGIExteriorCompute.get()), nullptr, 0);

	context->Dispatch(widthThreads, heightThreads, 1);

	srvs.fill(nullptr);
	uavs.fill(nullptr);
	cbs.fill(nullptr);
	//samplers.fill(nullptr);

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	//context->CSSetSamplers(3, (uint)samplers.size(), samplers.data());

	state->EndPerfEvent();
}

void VoxelConeTracingGI::DrawVCTGI()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] DrawVCTGI");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] DrawVCTGI");

	PostProcess();
	InjectLighting();
	ScreenConeTrace();

	state->EndPerfEvent();
}

void VoxelConeTracingGI::DebugDrawVoxels()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] DebugDrawVoxels");

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

	auto drawPixel = drawPixelVariants[settings.DebugDrawMode - 1];
	context->PSSetShader(drawPixel.get(), nullptr, 0);

	std::array<ID3D11ShaderResourceView*, 1> srvs = {
		clipmapVolumes[0]->srv.get()
	};

	if (settings.DebugDrawMode == DebugDrawMode::Lighting || settings.DebugDrawMode == DebugDrawMode::Final)
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
	state->BeginPerfEvent("[VCTGI] Inject Lighting");

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "[VCTGI] Inject Lighting");

	auto context = globals::d3d::context;
	
	const auto& lightingSettings = CurrentEnvironmentSettings().Lighting;

	// Collects lights for inection
	{
		lights.clear();
		lights.reserve(MAX_LIGHTS);

		auto accumulator = *globals::game::currentAccumulator.get();

		float directionalStrength = lightingSettings.Direct * lightingSettings.Directional;

		if (directionalStrength > 0.0f) 
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
				.color = float3(diffuse.red, diffuse.green, diffuse.blue) * directionalStrength,
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

							auto worldPos = niLight->world.translate;

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

		float omniStrength = lightingSettings.Direct * lightingSettings.Omni;

		if (omniStrength > 0.0f) {
			for (auto lightData: lightsData) {
				lights.push_back({ 
					.lvector = lightData.positionWS[0].data,
					.range = lightData.radius,
					.color = lightData.color * omniStrength,
					.type = 1
				});
			}
		}

		lightBuffer->Update(lights.data(), lights.size());
	}

	// Prepare indirect arguments buffer
	{
		state->BeginPerfEvent("Prepare indirect");

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			voxelIndirectArgsBuffer->uav.get()
		};

		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(postProcessIndirectArgsCompute.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);  // Copies voxelCount from 4 * 1 to 4 * 8 as ThreadGroupCountX = ceil(voxelCount / 64.0f)

		state->EndPerfEvent();
	}

	// Inject direct lighting into voxels
	{
		float size = static_cast<float>(settings.Size) / Util::Units::GAME_UNIT_TO_M;
		float3 sizeF3 = float3(size, size, size);

		float resolutionFloat = static_cast<float>(settings.Resolution);

		injectLightingCBData.Min = center - (sizeF3 * 0.5);
		injectLightingCBData.Size = size;
		injectLightingCBData.ResInv = 1.0f / resolutionFloat;
		injectLightingCBData.VoxelSize = size / resolutionFloat;
		injectLightingCBData.LightCount = std::min((uint)lights.size(), MAX_LIGHTS);
		injectLightingCBData.EmissiveMult = lightingSettings.Emissive;
		injectLightingCBData.AmbientMult = lightingSettings.Ambient;

		injectLightingCB->Update(injectLightingCBData);

		auto cb = injectLightingCB->CB();

		float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearUnorderedAccessViewFloat(clipmapVolumes[0]->uav.get(), clearColor);

		std::array<ID3D11ShaderResourceView*, 2> srvs = {
			lightBuffer->SRV(),
			voxelBuffer->SRV()
		};

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			clipmapVolumes[0]->uav.get()
		};

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetConstantBuffers(0, 1, &cb);

		context->CSSetShader(injectLightCompute.get(), nullptr, 0);
		context->DispatchIndirect(voxelIndirectArgsBuffer->resource.get(), 4 * 8); // Address of ceil(voxelCount / 64.0f)

		context->GenerateMips(clipmapVolumes[0]->srv.get());

		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	}

	state->EndPerfEvent();
}

void VoxelConeTracingGI::Prepass()
{

}

void VoxelConeTracingGI::PostPostLoad()
{
	Hooks::Install();
}

uint32_t VoxelConeTracingGI::GetVertexDescriptor(const RE::BSShader::Type& shaderType)
{
	uint32_t descriptor = 0;

	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC);
		break;
	case RE::BSShader::Type::Effect:
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::TexCoord);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Normals);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::MotionVectorsNormals);
		//descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::BinormalTangent);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Vc);
		//descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Skinned);
		//descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::TexCoordIndex);
		//descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Membrane);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Texture);
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

uint32_t VoxelConeTracingGI::GetPixelDescriptor(const RE::BSShader::Type& shaderType)
{
	uint32_t descriptor = 0;

	switch (shaderType) {
	case RE::BSShader::Type::Effect:
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Vc);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::TexCoord);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Normals);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Texture);
		descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::MotionVectorsNormals);
		//descriptor |= static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Membrane);
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
	case RE::BSShader::Type::Effect:
		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::TexCoord))
			defines.emplace_back("TEXCOORD", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Normals))
			defines.emplace_back("NORMALS", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::MotionVectorsNormals))
			defines.emplace_back("MOTIONVECTORS_NORMALS", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::BinormalTangent))
			defines.emplace_back("BINORMAL_TANGENT", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Vc))
			defines.emplace_back("VC", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Skinned))
			defines.emplace_back("SKINNED", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::TexCoordIndex))
			defines.emplace_back("TEXCOORD_INDEX", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Membrane))
			defines.emplace_back("MEMBRANE", "");

		if (descriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::Texture))
			defines.emplace_back("TEXTURE", "");			
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

void VoxelConeTracingGI::ClearShaderCache()
{
	ClearStaticShaders();
	ClearVCTShader();
}

void VoxelConeTracingGI::CompileShaders()
{
	CompileStaticShaders();
	CompileVCTShader();
}

void VoxelConeTracingGI::ClearStaticShaders()
{
	voxelizeVertex.clear();
	voxelizePixel.clear();
	voxelizeGeometry = nullptr;

	accumulateVertex = nullptr;
	accumulatePixel = nullptr;

	postProcessIndirectArgsCompute = nullptr;
	postProcessCompute = nullptr;

	injectLightCompute = nullptr;
	drawVertex = nullptr;

	for (auto& shader : drawPixelVariants)
		shader = nullptr;

	CompileStaticShaders();
}

void VoxelConeTracingGI::CompileStaticShaders()
{
	auto folderPath = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI");

	// Voxelization
	auto voxelizationFolderPath = folderPath / "Voxelize";

	// Lighting VS
	logger::info("Lighting VS");
	uint32_t lightingDescriptor = GetVertexDescriptor(RE::BSShader::Type::Lighting);
	const auto lightingDefines = GetShaderDefines<const char*>(RE::BSShader::Type::Lighting, lightingDescriptor);
	voxelizeVertex.emplace(RE::BSShader::Type::Lighting, ShaderUtils::ShaderPermutations<ID3D11VertexShader>(voxelizationFolderPath / "VoxelizeVS.Lighting.hlsl", lightingDefines));

	// Effect VS
	logger::info("Effect VS");
	uint32_t effectVSDescriptor = GetVertexDescriptor(RE::BSShader::Type::Effect);
	const auto effectVSDefines = GetShaderDefines<const char*>(RE::BSShader::Type::Effect, effectVSDescriptor);
	voxelizeVertex.emplace(RE::BSShader::Type::Effect, ShaderUtils::ShaderPermutations<ID3D11VertexShader>(voxelizationFolderPath / "VoxelizeVS.Effect.hlsl", effectVSDefines));

	// Lighting PS
	logger::info("Lighting PS");
	voxelizePixel.emplace(RE::BSShader::Type::Lighting, ShaderUtils::ShaderPermutations<ID3D11PixelShader>(voxelizationFolderPath / "VoxelizePS.hlsl", {}, { { "LIGHTING", "" } }));

	// Effect PS
	logger::info("Effect PS");
	uint32_t effectPSDescriptor = GetPixelDescriptor(RE::BSShader::Type::Effect);
	const auto effectPSDefines = GetShaderDefines<const char*>(RE::BSShader::Type::Effect, effectPSDescriptor);
	voxelizePixel.emplace(RE::BSShader::Type::Effect, ShaderUtils::ShaderPermutations<ID3D11PixelShader>(voxelizationFolderPath / "VoxelizePS.hlsl", effectPSDefines, { { "EFFECT", "" } }));

	CompileShader(&voxelizeGeometry, (voxelizationFolderPath / "VoxelizeGS.hlsl").c_str(), "gs_5_0");

	// Accumulation
	auto accumulateFolderPath = folderPath / "Accumulate";
	CompileShader(&accumulateVertex, (accumulateFolderPath / "AccumulateVS.hlsl").c_str(), "vs_5_0", {}, accumulateInputDesc, &accumulateInputLayout);
	CompileShader(&accumulatePixel, (accumulateFolderPath / "AccumulatePS.hlsl").c_str(), "ps_5_0");

	// Post procesing
	CompileShader(&postProcessIndirectArgsCompute, (folderPath / "PostProcessIndirectArgsCS.hlsl").c_str(), "cs_5_0");
	CompileShader(&postProcessCompute, (folderPath / "PostProcessCS.hlsl").c_str(), "cs_5_0");

	// Light injection
	CompileShader(&injectLightCompute, (folderPath / "InjectLightingCS.hlsl").c_str(), "cs_5_0");

	// Debug draw
	auto debugFolderpath = folderPath / "Debug";
	CompileShader(&drawVertex, (debugFolderpath / "DrawVS.hlsl").c_str(), "vs_5_0", {}, voxelInputDesc, &voxelDrawInputLayout);

	for (size_t i = 0; i < drawPixelVariants.size(); i++) {
		CompileShader(&drawPixelVariants[i], (debugFolderpath / "DrawPS.hlsl").c_str(), "ps_5_0", { { "DRAW_MODE", std::to_string(i).c_str() } });
	}
}

void VoxelConeTracingGI::ClearVCTShader()
{
	vctGIInteriorCompute = nullptr;
	vctGIExteriorCompute = nullptr;

	CompileVCTShader();
}

void VoxelConeTracingGI::CompileVCTShader()
{
	auto shaderPath = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI\\GlobalIlluminationCS.hlsl");

	CompileShader(&vctGIInteriorCompute, shaderPath.c_str(), "cs_5_0", { { "DIFFUSE_CONES", std::to_string(settings.Interior.Tracing.Diffuse.Count).c_str() } });
	CompileShader(&vctGIExteriorCompute, shaderPath.c_str(), "cs_5_0", { { "DIFFUSE_CONES", std::to_string(settings.Exterior.Tracing.Diffuse.Count).c_str() } });

	changedSettings &= ~ChangedSetting::ConeCount;
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