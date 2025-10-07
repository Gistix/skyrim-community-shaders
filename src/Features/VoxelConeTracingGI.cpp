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
	EnableVoxelConeTracingGI,
	NoIndoorDir,
	Lod0Resolution,
	Lod0Size)

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
		ImGui::TreePop();
	}

	if (ImGui::CollapsingHeader("Debug")) {

		if (ImGui::TreeNode("Debug Texture")) {
			ImGui::Image(debugTex->srv.get(), { debugTex->desc.Width * 0.5f, debugTex->desc.Height * 0.5f });
			ImGui::TreePop();
		}

		//ImGui::Text(std::format("Clustered Light Count : {}", lightCount).c_str());

		if (ImGui::TreeNodeEx("Trimesh Data", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const auto& item : shapeData) {
				auto& bufferData = item.second;
				ImGui::LabelText("Name", bufferData.name);
			}		

			ImGui::TreePop();
		}
	}
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

void VoxelConeTracingGI::BSLightingShader_SetupVoxelization(RE::BSShader* This, RE::BSRenderPass* Pass)
{
	if (This->shaderType != RE::BSShader::Type::Lighting)
		return;

	if (queuedReset) {
		shapeData.clear();
		queuedReset = false;
	}

	const auto geometry = Pass->geometry;
	const auto triShape = geometry->AsTriShape();

	if (shapeData.find(triShape) != shapeData.end())
		return;

	auto context = globals::d3d::context;

	float4x4 xmmTransform = GetXMFromNiTransform(triShape->world);
	float4x4 transform = xmmTransform.Transpose();

	/*for (uint row = 0; row < 3; ++row) {
		for (uint col = 0; col < 4; ++col) {
			transform[row * 4 + col] = xmmTransform.m[col][row];
		}
	}*/

	const auto& modelData = triShape->GetModelData().modelBound;

	const RE::NiPoint3 r = { modelData.radius, modelData.radius, modelData.radius };

	const RE::NiPoint3 aabbMin = modelData.center - r;
	const RE::NiPoint3 aabbMax = modelData.center + r;

	BufferData bufferData = {
		.name = geometry->name.c_str(),
		.transform = transform,
		.min = { aabbMin.x, aabbMin.y, aabbMin.z },
		.max = { aabbMax.x, aabbMax.y, aabbMax.z },
	};

	auto rendererData = triShape->GetGeometryRuntimeData().rendererData;

	if (rendererData != nullptr) {
		auto vertexBufferSrc = (ID3D11Buffer*)rendererData->vertexBuffer;
		D3D11_BUFFER_DESC vertexBufferDsc;
		vertexBufferSrc->GetDesc(&vertexBufferDsc);

		bufferData.vertexBuffer = eastl::make_unique<Buffer>(vertexBufferDsc);
		context->CopyResource(bufferData.vertexBuffer->resource.get(), vertexBufferSrc);

		auto indexBufferSrc = (ID3D11Buffer*)rendererData->indexBuffer;
		D3D11_BUFFER_DESC indexBufferDsc;
		indexBufferSrc->GetDesc(&indexBufferDsc);

		bufferData.indexBuffer = eastl::make_unique<Buffer>(indexBufferDsc);
		context->CopyResource(bufferData.indexBuffer->resource.get(), indexBufferSrc);
		shapeData.emplace(triShape, std::move(bufferData));
	}
}

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

	float lod0Size = static_cast<float>(settings.Lod0Size) / Util::Units::GAME_UNIT_TO_M;

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

	float lod0Size = static_cast<float>(settings.Lod0Size) / Util::Units::GAME_UNIT_TO_M;
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

RE::BSEventNotifyControl VoxelConeTracingGI::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell through a loadscreen, update every frame until completion
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			globals::features::voxelConeTracingGI.queuedReset = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}