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

static constexpr uint MAX_LIGHTS = 1024;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VoxelConeTracingGI::Settings,
	lod0Resolution,
	lod0Size)

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
	//ImGui::SeparatorText("Cheese");
	ImGui::Combo("Lod0 Resolution", &settings.lod0Resolution, dimensionsLabels, IM_ARRAYSIZE(dimensionsLabels));
	ImGui::InputInt("Lod0 Size", &settings.lod0Size);
}

void VoxelConeTracingGI::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		lightBuffer = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<Light>());
	}

	logger::debug("Creating Structured buffers...");
	{
		uint lod0MaxDepth = OctreeMaxDepth(0);
		uint lod0NodeCount = NodeCount(lod0MaxDepth);
		uint lod0LeafNodeCount = LeafNodeCount(lod0MaxDepth);

		// Node buffer
		eastl::vector<Node> nodes(lod0NodeCount);
		auto nodeBufferDesc = StructuredBufferDesc<Node>(lod0NodeCount, true, false);
		nodeBuffer = eastl::make_unique<StructuredBuffer>(nodeBufferDesc, nodes.data(), lod0NodeCount);
		nodeBuffer->CreateUAV(); // TODO: use D3D11_BUFFER_UAV_FLAG_APPEND :pray:

		// Voxel Buffer
		eastl::vector<Voxel> voxels(lod0LeafNodeCount);
		auto voxelBufferDesc = StructuredBufferDesc<Voxel>(lod0LeafNodeCount, true, false);
		voxelBuffer = eastl::make_unique<StructuredBuffer>(voxelBufferDesc, voxels.data(), lod0LeafNodeCount); // Line 75
		voxelBuffer->CreateUAV();
	}

	logger::debug("Creating textures...");
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
				.MipLevels = lod0TexDesc.MipLevels }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC lod0UAVDesc = {
			.Format = lod0TexDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = lod0TexDesc.Depth }
		};

		// Create textures
		{
			lod0Tex = eastl::make_unique<Texture3D>(lod0TexDesc);
			lod0Tex->CreateSRV(lod0SRVDesc);
			lod0Tex->CreateUAV(lod0UAVDesc);
		}
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

	CompileShaders();
}

void VoxelConeTracingGI::Voxelize()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Voxelize");
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
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Light Injection");

	//auto renderer = globals::game::renderer;
	//auto context = globals::d3d::context;

	lights.clear();
	lights.reserve(settings.maxLights);

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());
	auto sunRuntime = dirLight->GetLightRuntimeData();

	auto& directionNi = dirLight->GetWorldDirection();
	float3 directionF3 = { directionNi.x, directionNi.y, directionNi.z };
	directionF3.Normalize();

	auto diffuse = sunRuntime.diffuse;

	lights.push_back({
		.position = { directionF3.x, directionF3.y, directionF3.z },
		.color = { diffuse.red, diffuse.green, diffuse.blue },
		.type = 0,
	});

	const auto activeShadowSceneNode = accumulator->GetRuntimeData().activeShadowSceneNode;

	eastl::vector<LightLimitFix::LightData> lightsData{};
	lightsData.reserve(MAX_LIGHTS);

	auto& isl = globals::features::inverseSquareLighting;

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
						auto worldPos = niLight->world.translate - Util::GetEyePosition(0);
						light.positionWS[0].data.x = worldPos.x;
						light.positionWS[0].data.y = worldPos.y;
						light.positionWS[0].data.z = worldPos.z;

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
		//uint type = lightData.lightFlags.any(RE::TES_LIGHT_FLAGS::kSpotlight, RE::TES_LIGHT_FLAGS::kSpotShadow) ? 2 : 1;

		lights.push_back({ 
			.position = lightData.positionWS[0].data,
			.range = lightData.radius,
			.color = lightData.color,
		    .type = 1,
		});
	}
}

void VoxelConeTracingGI::Volumize()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Voxel Cone Tracing - Volumizer");
}

void VoxelConeTracingGI::Prepass()
{
	Voxelize();
	InjectLighting();
	Volumize();
}

void VoxelConeTracingGI::ClearShaderCache()
{
	cheeseCs = nullptr;  // This is actually optional
	CompileShaders();
}

void VoxelConeTracingGI::CompileShaders()
{
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VoxelConeTracingGI\\VoxelConeTracingGI.cs.hlsl", { { "SOME_MACRO", "0" } }, "cs_5_0")); rawPtr)
		cheeseCs.attach(rawPtr);
}