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
	Mode,
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
		// Mode
		{
			std::array<std::string, static_cast<size_t>(VoxelMode::Count)> items;

			const auto& itemsView = magic_enum::enum_names<VoxelMode>();
			for (size_t i = 0; i < items.size(); i++) {
				items[i] = std::string(itemsView[i]);
			}

			if (ImGui::BeginCombo("Mode", items[static_cast<size_t>(settings.Mode)].c_str())) {
				for (size_t i = 0; i < items.size(); i++) {
					bool isSelected = (static_cast<size_t>(settings.Mode) == i);

					if (ImGui::Selectable(items[i].c_str(), isSelected))
						settings.Mode = static_cast<VoxelMode>(i);

					if (isSelected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Voxel storage and sampling mode.\nStandard uses less memory and is faster but less accurate.\n");
			}
		}

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
		ImGui::SliderInt("Clipmaps", &settings.ClipmapCount, 1, MAX_CLIPMAPS);
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
	ImGui::PushID(coneName);

	if (ImGui::BeginTabItem(coneName)) {
		if (ImGui::TreeNodeEx("Step", ImGuiTreeNodeFlags_DefaultOpen)) {
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

	ImGui::PopID();
}

void VoxelConeTracingGI::DrawEnvironmentSettings(EnvironmentSettings& environmentSettings, const char* environmentName)
{
	ImGui::PushID(environmentName);

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

	ImGui::PopID();
}

const std::string& FormatSize(size_t bytes)
{
	static std::string buffer;

	const size_t KB = 1024;
	const size_t MB = KB * 1024;
	const size_t GB = MB * 1024;

	if (bytes >= GB) {
		double sizeGB = static_cast<double>(bytes) / GB;
		buffer = std::format("{:.2f} GB", sizeGB);
	} else if (bytes >= MB) {
		double sizeMB = static_cast<double>(bytes) / MB;
		buffer = std::format("{:.2f} MB", sizeMB);
	} else {
		buffer = std::format("{} B", bytes);
	}

	return buffer;
}


void VoxelConeTracingGI::DrawSettings()
{
	bool enabled = settings.Enabled;
	ImGui::Checkbox("Enable Voxel Cone Tracing GI", &enabled);
	settings.Enabled = enabled;

    if (!settings.Enabled) {
		ImGui::BeginDisabled(true);
	}

	DrawGeneralSettings();

	DrawEnvironmentSettings(settings.Interior, "Interior");
	DrawEnvironmentSettings(settings.Exterior, "Exterior");

	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::TreeNodeEx("Memory Usage", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text(std::format("3D Textures: {}", FormatSize(Statistics3DTextureMemory())).c_str());
			ImGui::Text(std::format("Buffers: {}", FormatSize(StatisticsBufferMemory())).c_str());
			ImGui::TreePop();
		}

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

		/*if (ImGui::TreeNode("Debug Texture")) {
			ImGui::Image(debugColorTexture->srv.get(), { debugColorTexture->desc.Width * 0.5f, debugColorTexture->desc.Height * 0.5f });
			ImGui::TreePop();
		}*/

		ImGui::TreePop();
	}

	if (!settings.Enabled)
		ImGui::EndDisabled(); 
}

size_t BitsPerPixel(DXGI_FORMAT fmt)
{
	switch (static_cast<int>(fmt)) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 128;

	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		return 96;

	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
	case DXGI_FORMAT_Y416:
	case DXGI_FORMAT_Y210:
	case DXGI_FORMAT_Y216:
		return 64;

	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
	case DXGI_FORMAT_R8G8_B8G8_UNORM:
	case DXGI_FORMAT_G8R8_G8B8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_AYUV:
	case DXGI_FORMAT_Y410:
	case DXGI_FORMAT_YUY2:
		return 32;

	case DXGI_FORMAT_P010:
	case DXGI_FORMAT_P016:
		return 24;

	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_A8P8:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		return 16;

	case DXGI_FORMAT_NV12:
	case DXGI_FORMAT_420_OPAQUE:
	case DXGI_FORMAT_NV11:
		return 12;

	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_AI44:
	case DXGI_FORMAT_IA44:
	case DXGI_FORMAT_P8:
		return 8;

	case DXGI_FORMAT_R1_UNORM:
		return 1;

	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return 4;

	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return 8;

	default:
		return 0;
	}
}

size_t VoxelConeTracingGI::Statistics3DTextureMemory()
{
	size_t bytes = 0;

	for (const auto& volumeTexture : clipmapsVolumeTexture) {
		if (volumeTexture != nullptr) {
			const auto& volumeTextureDesc = volumeTexture->desc;

			size_t bpp = BitsPerPixel(volumeTextureDesc.Format);
			size_t pixels = volumeTextureDesc.Width * volumeTextureDesc.Height * volumeTextureDesc.Depth;

			bytes += static_cast<size_t>(std::ceil((bpp * pixels) / 8.0));
		}
	}

	return bytes;
}

size_t VoxelConeTracingGI::StatisticsBufferMemory()
{
	return 0;
}

void VoxelConeTracingGI::SetupScreenTextures()
{
	logger::debug("Creating screen textures...");
	{
		const auto& renderer = globals::game::renderer;
		auto screenSize = renderer->GetScreenSize();

		// Diffuse GI and Specular GI output textures
		{
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

			diffuseGITexture = eastl::make_unique<Texture2D>(texDesc);
			diffuseGITexture->CreateSRV(srvDesc);
			diffuseGITexture->CreateUAV(uavDesc);
			diffuseGITexture->CreateRTV(rtvDesc);

			specularGITexture = eastl::make_unique<Texture2D>(texDesc);
			specularGITexture->CreateSRV(srvDesc);
			specularGITexture->CreateUAV(uavDesc);
			specularGITexture->CreateRTV(rtvDesc);
		}

		// Debug view textures
		{
			auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			auto mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto depthView = mainDepth.views[0];
		
			D3D11_TEXTURE2D_DESC colorDesc = {};
			main.texture->GetDesc(&colorDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			main.SRV->GetDesc(&srvDesc);

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			main.RTV->GetDesc(&rtvDesc);

			debugColorTexture = eastl::make_unique<Texture2D>(colorDesc);
			debugColorTexture->CreateSRV(srvDesc);
			debugColorTexture->CreateRTV(rtvDesc);

			D3D11_TEXTURE2D_DESC depthDesc = {};
			mainDepth.texture->GetDesc(&depthDesc);

			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};		
			depthView->GetDesc(&dsvDesc);

			debugDepthTexture = eastl::make_unique<Texture2D>(depthDesc);
			debugDepthTexture->CreateDSV(dsvDesc);
		}
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
			//.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
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

		clipmapsVolumeTexture.clear();

		for(int i=0; i < settings.ClipmapCount; i++) {
			auto clipmap = eastl::make_unique<Texture3D>(volumeTexDesc);

			clipmap->CreateSRV(volumeSRVDesc);
			clipmap->CreateUAV(volumeUAVDesc);

			clipmapsVolumeTexture.push_back(eastl::move(clipmap));
		}
	}
}

void VoxelConeTracingGI::SetupBuffers()
{
	uint resolution = settings.Resolution;
	uint maxVoxelCount = resolution * resolution * resolution;

	// Voxel Map Buffer - maps simulated append buffer to 1d voxel index
	{
		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = 4 * maxVoxelCount,
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
				.NumElements = maxVoxelCount,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW 
			}
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,  // DXGI_FORMAT_R32_UINT
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX,
			.BufferEx = {
				.FirstElement = 0,
				.NumElements = maxVoxelCount,
				.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW 
			}
		};

		clipmapsVoxelMapBuffer.clear();
		clipmapsVoxelMapBuffer.reserve(settings.ClipmapCount);

		for (int i = 0; i < settings.ClipmapCount; i++) {
			auto voxelMapBuffer = eastl::make_unique<Buffer>(bufferDesc);
			voxelMapBuffer->CreateUAV(uavDesc);
			voxelMapBuffer->CreateSRV(srvDesc);

			clipmapsVoxelMapBuffer.push_back(eastl::move(voxelMapBuffer));
		}
	}

	// Voxel mask buffer
	{
		uint numBits = maxVoxelCount;
		uint numBytes = (numBits + 7) / 8;
		uint maskElements = (numBytes + 3) / 4;

		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = 4 * maskElements,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = maskElements,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW }
		};

		clipmapsVoxelMaskBuffer.clear();
		clipmapsVoxelMaskBuffer.reserve(settings.ClipmapCount);

		for (int i = 0; i < settings.ClipmapCount; i++) {
			auto voxelMaskBuffer = eastl::make_unique<Buffer>(bufferDesc);
			voxelMaskBuffer->CreateUAV(uavDesc);

			clipmapsVoxelMaskBuffer.push_back(eastl::move(voxelMaskBuffer));
		}
	}

	// Voxel Count Buffer (allows voxel structured buffer to mimic append)
	{
		D3D11_BUFFER_DESC countBufferDesc = {
			.ByteWidth = 4 * MAX_CLIPMAPS,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = MAX_CLIPMAPS,
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW 
			}
		};

		voxelCountBuffer = eastl::make_unique<Buffer>(countBufferDesc);
		voxelCountBuffer->CreateUAV(uavDesc);
	}

	// Voxel Buffer
	{
		auto voxelBufferDesc = StructuredBufferDesc<Voxel>(maxVoxelCount, true, false);

		clipmapsVoxelBuffer.clear();
		clipmapsVoxelBuffer.reserve(settings.ClipmapCount);

		for (int i = 0; i < settings.ClipmapCount; i++) {
			auto voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, maxVoxelCount);
			voxelBuffer->CreateUAV();
			voxelBuffer->CreateSRV();

			clipmapsVoxelBuffer.push_back(eastl::move(voxelBuffer));
		}
	}
}

void VoxelConeTracingGI::SetupViewport()
{
	float resolutionFloat = static_cast<float>(settings.Resolution);

	voxelizeViewport.TopLeftX = 0.0f;
	voxelizeViewport.TopLeftY = 0.0f;
	voxelizeViewport.Width = resolutionFloat;
	voxelizeViewport.Height = resolutionFloat;
	voxelizeViewport.MinDepth = 0.0f;
	voxelizeViewport.MaxDepth = 1.0f;
}


void VoxelConeTracingGI::SetupResources()
{
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->QueryInterface(__uuidof(ID3D11Device3), device3.put_void()));

	logger::debug("Creating Constant buffers...");
	{
		voxelConeTracingGICB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VoxelConeTracingGICB>());	

		vsFrameBufferPSCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VSFrameBufferPSCB>());
		clipmapCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<ClipmapCB>());

		injectLightingCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<InjectLightingCB>());

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
		UINT indirectArgsInit[3 * MAX_CLIPMAPS];

		for (uint i = 0; i < MAX_CLIPMAPS; i++) {
			const uint offset = i * 3;

			indirectArgsInit[offset] = 0;
			indirectArgsInit[offset + 1] = 1;
			indirectArgsInit[offset + 2] = 1;
		}

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

	{
		UINT init[4 * MAX_CLIPMAPS];

		for (uint i = 0; i < MAX_CLIPMAPS; i++) {
			const uint offset = i * 4;

			init[offset] = 36;
			init[offset + 1] = 0;
			init[offset + 2] = 0;
			init[offset + 3] = 0;
		}

		uint byteWidth = sizeof(init);

		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = byteWidth,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
		};

		D3D11_SUBRESOURCE_DATA initData = {};
		initData.pSysMem = init;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = 0,
				.NumElements = byteWidth / sizeof(init[0]),
				.Flags = D3D11_BUFFER_UAV_FLAG_RAW }
		};

		voxelDrawIndirectArgsBuffer = eastl::make_unique<Buffer>(bufferDesc, &initData);
		voxelDrawIndirectArgsBuffer->CreateUAV(uavDesc);	
	}

	// Input Description
	{
		voxelInputDesc = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC volumeSamplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&volumeSamplerDesc, volumeSampler.put()));
	}

	// Raster State
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
		DX::ThrowIfFailed(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)));

		logger::warn("CheckFeatureSupport: ConservativeRasterizationTier: {}", magic_enum::enum_name(options2.ConservativeRasterizationTier));
		logger::warn("CheckFeatureSupport: ROVsSupported: {}", options2.ROVsSupported);

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

bool IntersectSphereAABB(const float3& center, float radius, const float3& boxMin, const float3& boxMax)
{
	// Find closest point on the AABB to the sphere center
	float3 closest;
	closest.x = fmax(boxMin.x, fmin(center.x, boxMax.x));
	closest.y = fmax(boxMin.y, fmin(center.y, boxMax.y));
	closest.z = fmax(boxMin.z, fmin(center.z, boxMax.z));

	// Compute squared distance
	float dx = closest.x - center.x;
	float dy = closest.y - center.y;
	float dz = closest.z - center.z;
	float dist2 = dx * dx + dy * dy + dz * dz;

	return dist2 <= radius * radius;
}

bool ContainedSphereAABB(const float3& center, float radius, const float3& boxMin, const float3& boxMax)
{
	return (center.x - radius >= boxMin.x && center.x + radius <= boxMax.x) &&
	       (center.y - radius >= boxMin.y && center.y + radius <= boxMax.y) &&
	       (center.z - radius >= boxMin.z && center.z + radius <= boxMax.z);
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

				/*const auto& transform = geometry->world;

				const auto& modelData = triShape->GetModelData().modelBound;

				const auto& modelCenterTransformed = transform * modelData.center;
				const auto& modelCenter = float3(modelCenterTransformed.x, modelCenterTransformed.y, modelCenterTransformed.z);

				const auto& r = float3(modelData.radius, modelData.radius, modelData.radius);

				const auto& aabbMin = modelCenter - r;
				const auto& aabbMax = modelCenter + r;

				if (!IntersectAABB(aabbMin, aabbMax, lastClipmap.Min, lastClipmap.Max)) {
					skipTriShape = true;
				}*/

				const auto& lastClipmap = voxelConeTracingGICBData.Clipmaps[voxelConeTracingGICBData.ClipmapCount - 1];

				const auto& worldBound = geometry->worldBound;
				const auto& center = float3(worldBound.center.x, worldBound.center.y, worldBound.center.z);

				if (!IntersectSphereAABB(center, worldBound.radius, lastClipmap.Min, lastClipmap.Max)) {
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

	// Lets back everything up
	uint numViewports = 1;
	D3D11_VIEWPORT prevViewport = {};
	context->RSGetViewports(&numViewports, &prevViewport);

	ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

	context->VSSetShader(vertexShader.get(), nullptr, 0);
	context->GSSetShader(voxelizeGeometry.get(), nullptr, 0);
	context->PSSetShader(pixelShader.get(), nullptr, 0);

	context->RSSetState(voxelRasterState.get());

	context->RSSetViewports(1, &voxelizeViewport);

	// Clipmap handling
	/*const auto& transform = geometry->world;

	const auto& modelData = triShape->GetModelData().modelBound;

	const auto& modelCenterTransformed = transform * modelData.center;
	const auto& modelCenter = float3(modelCenterTransformed.x, modelCenterTransformed.y, modelCenterTransformed.z);

	const auto& r = float3(modelData.radius, modelData.radius, modelData.radius);

	const auto& aabbMin = modelCenter - r;
	const auto& aabbMax = modelCenter + r;*/

	const auto& worldBound = geometry->worldBound;
	const auto& center = float3(worldBound.center.x, worldBound.center.y, worldBound.center.z);

	std::vector<uint> renderClipmap;
	renderClipmap.reserve(voxelConeTracingGICBData.ClipmapCount);

	// Samples UAV
	std::array<ID3D11UnorderedAccessView*, (MAX_UAV_CLIPMAPS * 3) + 1> uavs;
	uavs.fill(nullptr);

	for (uint i = 0; i < voxelConeTracingGICBData.ClipmapCount; i++)
	{
		const auto& clipmap = voxelConeTracingGICBData.Clipmaps[i];

		bool contains = ContainedSphereAABB(center, worldBound.radius, clipmap.Min, clipmap.Max);
		bool intersects = IntersectSphereAABB(center, worldBound.radius, clipmap.Min, clipmap.Max);

		if (contains || intersects) {
			const uint& uavClipIndex = (uint)renderClipmap.size();

			// UAV limits us to 2 clips per draw
			if (uavClipIndex > 1)
				break;

			renderClipmap.push_back(i);

			uavs[uavClipIndex] = clipmapsVoxelBuffer[i]->UAV();
			uavs[MAX_UAV_CLIPMAPS + uavClipIndex] = clipmapsVoxelMaskBuffer[i]->uav.get();
			uavs[(MAX_UAV_CLIPMAPS * 2) + uavClipIndex] = clipmapsVoxelMapBuffer[i]->uav.get();

			// Model is contained completely inside and doesn't intersect any other clipmap 
			if (contains) {
				break;
			}
		}
	}

	uavs[uavs.size() - 1] = voxelCountBuffer->uav.get();

	// Render
	uint indexCount = triShapeRuntime.triangleCount * 3;
	//context->DrawIndexed(indexCount, 0, 0);

	uint instanceCount = (uint)renderClipmap.size();
	uint firstInstance = renderClipmap.front();

	// Set clip offset for UAV array
	clipmapCBData.Start = firstInstance;
	clipmapCB->Update(clipmapCBData);
	auto cb = clipmapCB->CB();

	context->GSSetConstantBuffers(1, 1, &cb);
	context->PSSetConstantBuffers(13, 1, &cb);

	// Set samples UAV
	context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, (uint)uavs.size(), uavs.data(), nullptr);

	// Render
	context->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);

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

	//context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);
	context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, (uint)uavs.size(), uavs.data(), nullptr);

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i]) {
			prevRTVs[i]->Release();
		}
	}

	if (prevDSV) {
		prevDSV->Release();
	}

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

		uint resolution = settings.Resolution;
		float resolutionFloat = static_cast<float>(resolution);

		const auto& avgEyePosition = Util::GetAverageEyePosition();

		voxelConeTracingGICBData.Res = resolution;
		voxelConeTracingGICBData.ResRcp = 10000.0f / resolutionFloat;

		voxelConeTracingGICBData.ClipmapCount = static_cast<uint>(settings.ClipmapCount);
		
		for (int i = 0; i < settings.ClipmapCount; i++)
		{
			float clipSize = static_cast<float>(settings.Size * pow(settings.ScaleFactor, i)) / Util::Units::GAME_UNIT_TO_M;
			float3 clipExents = float3(clipSize, clipSize, clipSize) * 0.5f;

			float clipVoxelSize = clipSize / resolutionFloat;

			const auto& clipCenter = float3(
				floor(avgEyePosition.x / clipVoxelSize) * clipVoxelSize,
				floor(avgEyePosition.y / clipVoxelSize) * clipVoxelSize,
				floor(avgEyePosition.z / clipVoxelSize) * clipVoxelSize);

			// + float3(voxelSize * 0.5f, voxelSize * 0.5f, voxelSize * 0.5f);

			voxelConeTracingGICBData.Clipmaps[i] = {
				.Min = clipCenter - clipExents,
				.Size = clipSize,
				.Max = clipCenter + clipExents,
				.SizeRcp = 10000.0f / clipSize
			};
		}

		voxelConeTracingGICB->Update(voxelConeTracingGICBData);

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

		auto vxGICB = voxelConeTracingGICB->CB();

		auto context = globals::d3d::context;
		
		context->VSSetConstantBuffers(13, 1, &vsFBPSCB);
		context->GSSetConstantBuffers(0, 1, &vxGICB);

		// Clear/reset buffers
		{
			UINT clearValues[4] = { 0, 0, 0, 0 };

			for (uint i = 0; i < voxelConeTracingGICBData.ClipmapCount; i++)
			{				
				context->ClearUnorderedAccessViewUint(clipmapsVoxelMapBuffer[i]->uav.get(), clearValues);
				context->ClearUnorderedAccessViewUint(clipmapsVoxelMaskBuffer[i]->uav.get(), clearValues);			
			}

			context->ClearUnorderedAccessViewUint(voxelCountBuffer->uav.get(), clearValues);	
		}

		renderingWorld = true;
	}

	//logger::info("Main_RenderWorld: {}", globals::state->frameCount);

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

void VoxelConeTracingGI::ScreenConeTrace()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] ScreenConeTrace");

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
		clipmapsVolumeTexture[0]->srv.get()
	};

	std::array<ID3D11UnorderedAccessView*, 2> uavs = {
		diffuseGITexture->uav.get(),
		specularGITexture->uav.get()
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

	/*std::array<ID3D11SamplerState*, 1> samplers = {
		volumeSampler.get()
	};
	context->CSSetSamplers(3, (uint)samplers.size(), samplers.data());*/

	ID3D11SamplerState* sampler = volumeSampler.get();
	context->CSSetSamplers(0, 1, &sampler);

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

	//logger::info("DrawVCTGI: {}", globals::state->frameCount);

	InjectLighting();
	ScreenConeTrace();

	state->EndPerfEvent();
}

void VoxelConeTracingGI::DebugDrawVoxels()
{
	auto state = globals::state;
	state->BeginPerfEvent("[VCTGI] DebugDrawVoxels");

	//logger::info("DebugDrawVoxels: {}", globals::state->frameCount);

	const auto& context = globals::d3d::context;
	const auto& renderer = globals::game::renderer;

	// Lets back everything up
	ID3D11RenderTargetView* prevRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, &prevDSV);

	// Clear Color and Depth
	/*float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	context->ClearRenderTargetView(debugColorTexture->rtv.get(), clearColor);
	context->ClearDepthStencilView(debugDepthTexture->dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);*/

	context->VSSetShader(drawVertex.get(), nullptr, 0);
	//context->GSSetShader(drawGeometry.get(), nullptr, 0);

	auto drawPixel = drawPixelVariants[settings.DebugDrawMode - 1];
	context->PSSetShader(drawPixel.get(), nullptr, 0);

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

	/*auto debugColorRTV = debugColorTexture->rtv.get();
	context->OMSetRenderTargets(1, &debugColorRTV, debugDepthTexture->dsv.get());*/

	/*auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];*/

	/*auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	context->OMSetRenderTargets(1, &main.RTV, mainDepth.views[0]);*/

	/*auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto depthView = depth.views[0];

	auto depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];*/

	// Clear Color and Depth
	/*float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	context->ClearRenderTargetView(main.RTV, clearColor);
	context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);*/

	context->IASetInputLayout(voxelDrawInputLayout.get());

	const auto& vertexBuffer = voxelDrawVertexBuffer->resource.get();
	UINT stride = sizeof(float3);
	UINT offset = 0;

	context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

	for (uint j = 0; j < voxelConeTracingGICBData.ClipmapCount; j++) {
		uint i = (voxelConeTracingGICBData.ClipmapCount - 1) - j;

		state->BeginPerfEvent(std::format("[VCTGI] DebugDrawVoxels - Clipmap {}", i).c_str());

		const auto& voxelBuffer = clipmapsVoxelBuffer[i];

		const auto& voxelBufferSRV = voxelBuffer->SRV();
		context->VSSetShaderResources(0, 1, &voxelBufferSRV);

		clipmapCBData.Index = i;
		clipmapCB->Update(clipmapCBData);

		std::array<ID3D11Buffer*, 2> cbs = {
			voxelConeTracingGICB->CB(),
			clipmapCB->CB()
		};

		context->VSSetConstantBuffers(0, (uint)cbs.size(), cbs.data());
		context->GSSetConstantBuffers(0, (uint)cbs.size(), cbs.data());

		if (settings.DebugDrawMode == DebugDrawMode::Lighting || settings.DebugDrawMode == DebugDrawMode::Final) {
			const auto& volumeTexture = clipmapsVolumeTexture[i];
			ID3D11ShaderResourceView* srv = volumeTexture->srv.get();
			context->PSSetShaderResources(0, 1, &srv);
		}

		context->DrawInstancedIndirect(voxelDrawIndirectArgsBuffer->resource.get(), 4 * (i * 4));

		state->EndPerfEvent();
	}

	/*auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];*/

	//context->CopyResource(main.texture, debugColorTexture->resource.get());
	//context->CopyResource(mainDepth.texture, debugDepthTexture->resource.get());

	//context->CopyResource(depthCopy.texture, depth.texture);

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRTVs, prevDSV);

	for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		if (prevRTVs[i]) {
			prevRTVs[i]->Release();
		}
	}

	if (prevDSV) {
		prevDSV->Release();
	}

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
		state->BeginPerfEvent("[VCTGI] Prepare indirect");

		std::array<ID3D11UnorderedAccessView*, 3> uavs = {
			voxelCountBuffer->uav.get(),
			voxelIndirectArgsBuffer->uav.get(),
			voxelDrawIndirectArgsBuffer->uav.get()
		};

		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(indirectArgsCompute.get(), nullptr, 0);
		context->Dispatch(voxelConeTracingGICBData.ClipmapCount, 1, 1);

		uavs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		state->EndPerfEvent();
	}

	{
		injectLightingCBData.LightCount = std::min((uint)lights.size(), MAX_LIGHTS);
		injectLightingCBData.EmissiveMult = lightingSettings.Emissive;
		injectLightingCBData.AmbientMult = lightingSettings.Ambient;
		injectLightingCB->Update(injectLightingCBData);	
	}

	// Inject direct lighting into voxels
	for (uint i = 0; i < voxelConeTracingGICBData.ClipmapCount; i++) {
		state->BeginPerfEvent(std::format("[VCTGI] Inject Light - Clipmap {}", i).c_str());

		const auto& volumeTexture = clipmapsVolumeTexture[i];
		const auto& voxelBuffer = clipmapsVoxelBuffer[i];

		clipmapCBData.Index = i;
		clipmapCB->Update(clipmapCBData);

		std::array<ID3D11Buffer*, 3> cbs = {
			voxelConeTracingGICB->CB(),
			clipmapCB->CB(),
			injectLightingCB->CB()
		};

		float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearUnorderedAccessViewFloat(volumeTexture->uav.get(), clearColor);

		std::array<ID3D11ShaderResourceView*, 2> srvs = {
			lightBuffer->SRV(),
			voxelBuffer->SRV()
		};

		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			volumeTexture->uav.get()
		};

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetConstantBuffers(0, (uint)cbs.size(), cbs.data());

		context->CSSetShader(injectLightCompute.get(), nullptr, 0);
		context->DispatchIndirect(voxelIndirectArgsBuffer->resource.get(), 4 * (i * 3)); // Address of ceil(voxelCount / 64.0f)

		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->GenerateMips(volumeTexture->srv.get());

		state->EndPerfEvent();
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

	indirectArgsCompute = nullptr;
	postProcessCompute = nullptr;

	injectLightCompute = nullptr;
	drawVertex = nullptr;
	drawGeometry = nullptr;
	for (auto& shader : drawPixelVariants)
		shader = nullptr;

	CompileStaticShaders();
}

void VoxelConeTracingGI::CompileStaticShaders()
{
	auto folderPath = std::filesystem::path("Data\\Shaders\\VoxelConeTracingGI");

	// Voxelization
	{
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
	}
	
	// Voxel count to indirect args
	CompileShader(&indirectArgsCompute, (folderPath / "IndirectArgsCS.hlsl").c_str(), "cs_5_0");

	// Light injection
	CompileShader(&injectLightCompute, (folderPath / "InjectLightingCS.hlsl").c_str(), "cs_5_0");

	// Debug draw
	auto debugFolderpath = folderPath / "Debug";
	CompileShader(&drawVertex, (debugFolderpath / "DrawVS.hlsl").c_str(), "vs_5_0", {}, voxelInputDesc, &voxelDrawInputLayout);

	CompileShader(&drawGeometry, (debugFolderpath / "DrawGS.hlsl").c_str(), "gs_5_0");

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