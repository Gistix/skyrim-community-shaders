#include "Raytracing.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/PathTracing.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.raytracing."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Raytracing::Settings,
	CreationEngineRaytracingSettings,
	RendererSettings)

void Raytracing::RestoreDefaultSettings()
{
	settings = {};
}

void Raytracing::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateSettings();
}

void Raytracing::SaveSettings(json& o_json)
{
	o_json = settings;
}

bool Raytracing::Available(bool a_initialized) const
{
	if (forcedDisabled || !loaded)
		return false;
	if (a_initialized && !initialized)
		return false;
	return settings.CreationEngineRaytracingSettings.Enabled;
}

CreationEngineRaytracing::Settings Raytracing::GetSettings() const
{
	auto certSettings = settings.CreationEngineRaytracingSettings;

	if (globals::features::pathTracing.loaded && globals::features::pathTracing.settings.Enabled) {
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::PathTracing;
		certSettings.GeneralSettings.Denoiser = globals::features::pathTracing.settings.GeneralSettings.Denoiser;
		certSettings.RaytracingSettings = globals::features::pathTracing.settings.RaytracingSettings;
	} else {
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::None;
	}

	return certSettings;
}

CreationEngineRaytracing::Mode Raytracing::Mode() const
{
	if (!Available())
		return CreationEngineRaytracing::Mode::None;
	if (globals::features::pathTracing.loaded && globals::features::pathTracing.settings.Enabled)
		return CreationEngineRaytracing::Mode::PathTracing;
	return settings.CreationEngineRaytracingSettings.GeneralSettings.Mode;
}

bool Raytracing::IsPathTracing() const
{
	return Mode() == CreationEngineRaytracing::Mode::PathTracing;
}

void Raytracing::UpdateSettings()
{
	if (creationEngineRaytracing && creationEngineRaytracing->UpdateSettings) {
		creationEngineRaytracing->UpdateSettings(GetSettings());
	}
}

void Raytracing::Execute()
{
	if (!Available() || Mode() == CreationEngineRaytracing::Mode::None)
		return;

	if (auto* dxvk = DXVKInterop::GetSingleton()) {
		if (auto* interopDevice = dxvk->GetInteropDevice()) {
			interopDevice->FlushRenderingCommands();
		}
	}

	creationEngineRaytracing->Execute();
	const uint32_t completedSlot = creationEngineRaytracing->PostExecution();

	if (completedSlot >= CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT)
		return;

	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto* context = globals::d3d::context;
	if (!context)
		return;

	const auto& renderTargets = renderer->GetRuntimeData().renderTargets;
	auto& main = renderTargets[RE::RENDER_TARGETS::kMAIN];

	const bool pathtracing = (Mode() == CreationEngineRaytracing::Mode::PathTracing);
	const bool debug = (Mode() == CreationEngineRaytracing::Mode::Debug);

	if (pathtracing || debug) {
		static bool loggedPointersOnce = false;
		if (!loggedPointersOnce) {
			loggedPointersOnce = true;
			auto& mv = renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			auto depthStencils = renderer->GetDepthStencilData().depthStencils;
			auto& mainDepth = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& mainDepthCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
			auto& zPrePassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

			logger::info("[Raytracing] Execute Pointers Check:");
			logger::info("  main.texture: {}, main.UAV: {}", static_cast<void*>(main.texture), static_cast<void*>(main.UAV));
			logger::info("  mv.texture: {}, mv.UAV: {}", static_cast<void*>(mv.texture), static_cast<void*>(mv.UAV));
			logger::info("  mainDepth.views[0]: {}, mainDepth.texture: {}", static_cast<void*>(mainDepth.views[0]), static_cast<void*>(mainDepth.texture));
			logger::info("  mainDepthCopy.views[0]: {}, mainDepthCopy.texture: {}", static_cast<void*>(mainDepthCopy.views[0]), static_cast<void*>(mainDepthCopy.texture));
			logger::info("  zPrePassCopy.views[0]: {}, zPrePassCopy.texture: {}", static_cast<void*>(zPrePassCopy.views[0]), static_cast<void*>(zPrePassCopy.texture));
			logger::info("  sharedMainTextures[{}].srv: {}", completedSlot, static_cast<void*>(sharedMainTextures[completedSlot].srv.get()));
			logger::info("  sharedMotionVectorTextures[{}].srv: {}", completedSlot, static_cast<void*>(sharedMotionVectorTextures[completedSlot].srv.get()));
			logger::info("  sharedDepthTextures[{}].srv: {}", completedSlot, static_cast<void*>(sharedDepthTextures[completedSlot].srv.get()));
			logger::info("  ptCompositeCS: {}, copyDepthVS: {}, copyDepthPS: {}", static_cast<void*>(ptCompositeCS.get()), static_cast<void*>(copyDepthVS.get()), static_cast<void*>(copyDepthPS.get()));
			D3D11_TEXTURE2D_DESC mainDesc{}, mvDesc{};
			if (main.texture) main.texture->GetDesc(&mainDesc);
			if (mv.texture) mv.texture->GetDesc(&mvDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC mainUavDesc{}, mvUavDesc{};
			if (main.UAV) main.UAV->GetDesc(&mainUavDesc);
			if (mv.UAV) mv.UAV->GetDesc(&mvUavDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC mainSrvDesc{}, mvSrvDesc{}, depthSrvDesc{};
			if (sharedMainTextures[completedSlot].srv) sharedMainTextures[completedSlot].srv->GetDesc(&mainSrvDesc);
			if (sharedMotionVectorTextures[completedSlot].srv) sharedMotionVectorTextures[completedSlot].srv->GetDesc(&mvSrvDesc);
			if (sharedDepthTextures[completedSlot].srv) sharedDepthTextures[completedSlot].srv->GetDesc(&depthSrvDesc);

			logger::info("[Raytracing] Formats Check:");
			logger::info("  main.texture format: {}, main.UAV format: {}", (int)mainDesc.Format, (int)mainUavDesc.Format);
			logger::info("  mv.texture format: {}, mv.UAV format: {}", (int)mvDesc.Format, (int)mvUavDesc.Format);
			logger::info("  sharedMain SRV format: {}", (int)mainSrvDesc.Format);
			logger::info("  sharedMV SRV format: {}", (int)mvSrvDesc.Format);
			logger::info("  sharedDepth SRV format: {}", (int)depthSrvDesc.Format);
		}

		float2 screenSize{ static_cast<float>(globals::game::graphicsState->screenWidth), static_cast<float>(globals::game::graphicsState->screenHeight) };
		auto dynamicScreenSize = Util::ConvertToDynamic(screenSize);

		if (screenCB && screenData) {
			screenData->Resolution = { static_cast<uint32_t>(screenSize.x), static_cast<uint32_t>(screenSize.y) };
			screenData->DynamicResolution = { static_cast<uint32_t>(dynamicScreenSize.x), static_cast<uint32_t>(dynamicScreenSize.y) };
			screenCB->Update(screenData.get(), sizeof(ScreenData));
		}

		// Blend pathtracing and sky (colors and motion vectors)
		if (ptCompositeCS && screenCB && sharedMainTextures[completedSlot].srv && sharedMotionVectorTextures[completedSlot].srv) {
			auto& mv = renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			context->CSSetShader(ptCompositeCS.get(), nullptr, 0);

			ID3D11Buffer* cb = screenCB->CB();
			context->CSSetConstantBuffers(0, 1, &cb);

			ID3D11ShaderResourceView* srvs[] = {
				sharedMainTextures[completedSlot].srv.get(),
				sharedMotionVectorTextures[completedSlot].srv.get()
			};
			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11UnorderedAccessView* uavs[] = {
				main.UAV,
				mv.UAV
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			auto dispatchCount = Util::GetScreenDispatchCount(true);
			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

			uavs[0] = nullptr;
			uavs[1] = nullptr;
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		} else if (sharedMainTextures[completedSlot].texture.shared && main.texture) {
			context->CopyResource(main.texture, sharedMainTextures[completedSlot].texture.shared);
		}

		// Copy Depth buffer
		if (copyDepthVS && copyDepthPS && sharedDepthTextures[completedSlot].srv) {
			auto depthStencils = renderer->GetDepthStencilData().depthStencils;

			auto& mainDepth = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& mainDepthCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
			auto& zPrePassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

			context->ClearDepthStencilView(mainDepth.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
			context->ClearDepthStencilView(mainDepthCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);
			context->ClearDepthStencilView(zPrePassCopy.views[0], D3D11_CLEAR_DEPTH, 1.0f, 0u);

			ID3D11DepthStencilState* oldDSS = nullptr;
			UINT oldRef = 0;
			context->OMGetDepthStencilState(&oldDSS, &oldRef);

			ID3D11RenderTargetView* oldRTV = nullptr;
			ID3D11DepthStencilView* oldDSV = nullptr;
			context->OMGetRenderTargets(1, &oldRTV, &oldDSV);

			UINT numViewports = 1;
			D3D11_VIEWPORT oldViewport = {};
			context->RSGetViewports(&numViewports, &oldViewport);

			D3D11_VIEWPORT viewport = {};
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = screenSize.x;
			viewport.Height = screenSize.y;
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			context->RSSetViewports(1, &viewport);

			context->IASetInputLayout(nullptr);
			context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			context->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);
			context->OMSetDepthStencilState(depthStencilState.get(), 0);

			context->RSSetState(copyRasterizerState.get());
			context->OMSetBlendState(copyBlendState.get(), nullptr, 0xffffffff);

			context->VSSetShader(copyDepthVS.get(), nullptr, 0);
			context->PSSetShader(copyDepthPS.get(), nullptr, 0);

			ID3D11ShaderResourceView* srvs[] = { sharedDepthTextures[completedSlot].srv.get() };
			context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			context->Draw(3, 0);

			context->OMSetDepthStencilState(oldDSS, oldRef);
			context->OMSetRenderTargets(1, &oldRTV, oldDSV);
			context->RSSetViewports(1, &oldViewport);

			if (oldDSS) {
				oldDSS->Release();
				oldDSS = nullptr;
			}
			if (oldRTV) {
				oldRTV->Release();
				oldRTV = nullptr;
			}
			if (oldDSV) {
				oldDSV->Release();
				oldDSV = nullptr;
			}

			context->PSSetShader(nullptr, nullptr, 0);
			context->VSSetShader(nullptr, nullptr, 0);

			context->CopyResource(mainDepthCopy.texture, mainDepth.texture);
			context->CopyResource(zPrePassCopy.texture, mainDepth.texture);
		}
	} else if (sharedMainTextures[completedSlot].texture.shared && main.texture) {
		context->CopyResource(main.texture, sharedMainTextures[completedSlot].texture.shared);
	}
}

void Raytracing::Load()
{
	if (forcedDisabled)
		return;

	Hooks::Install();
}

void Raytracing::PostPostLoad()
{
	creationEngineRaytracing = std::make_unique<CreationEngineRaytracing>();

	if (!creationEngineRaytracing->handle) {
		settings.CreationEngineRaytracingSettings.Enabled = false;
		forcedDisabled = true;
		disableReason = DisableReason::MissingPlugin;
		logger::warn("[Raytracing] 'CreationEngineRaytracing.dll' not found, feature disabled.");
		return;
	}

	logger::info("[Raytracing] Loaded 'CreationEngineRaytracing.dll' module successfully.");
}

bool Raytracing::InitializeCERaytracing()
{
	if (forcedDisabled || initialized)
		return false;

	if (!creationEngineRaytracing || !creationEngineRaytracing->handle)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (dxvk && (dxvk->IsAvailable() || dxvk->Initialize())) {
		VkQueue graphicsQueue = dxvk->GetQueue();
		uint32_t queueIndex = 0;
		uint32_t queueFamilyIndex = dxvk->GetQueueFamilyIndex();
		dxvk->GetSubmissionQueue1(&graphicsQueue, &queueIndex, &queueFamilyIndex);

		bool result = creationEngineRaytracing->InitializeVulkanRenderer(
			&settings.RendererSettings,
			dxvk->GetInstance(),
			dxvk->GetPhysicalDevice(),
			dxvk->GetDevice(),
			graphicsQueue,
			static_cast<int>(queueIndex),
			graphicsQueue,
			static_cast<int>(queueIndex),
			graphicsQueue,
			static_cast<int>(queueIndex));

		if (!result) {
			settings.CreationEngineRaytracingSettings.Enabled = false;
			initialized = false;
			forcedDisabled = true;
			disableReason = DisableReason::InitFailed;
			logger::error("[Raytracing] Failed to initialize Creation Engine ray tracing via Vulkan renderer.");
			return false;
		}

		initialized = true;
		UpdateResolution();
		logger::info("[Raytracing] Successfully initialized Creation Engine ray tracing (Vulkan).");
		return true;
	}

	logger::warn("[Raytracing] Renderer backend unavailable or incompatible for Creation Engine ray tracing initialization.");
	return false;
}

bool Raytracing::UpdateResolution()
{
	if (!globals::game::graphicsState || !creationEngineRaytracing || !creationEngineRaytracing->SetResolution)
		return false;

	uint2 resolution{
		globals::game::graphicsState->screenWidth,
		globals::game::graphicsState->screenHeight
	};

	if (resolution == m_Resolution)
		return false;

	m_Resolution = resolution;
	creationEngineRaytracing->SetResolution(m_Resolution.x, m_Resolution.y);
	return true;
}

void Raytracing::SetupResources()
{
	if (forcedDisabled)
		return;

	if (!initialized)
		InitializeCERaytracing();

	if (!initialized)
		return;

	creationEngineRaytracing->Initialize(GetSettings());

	auto* device = globals::d3d::device;
	if (!screenCB)
		screenCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ScreenData>());
	if (!screenData)
		screenData = std::make_unique<ScreenData>();

	if (device) {
		if (!copyBlendState) {
			D3D11_BLEND_DESC blendDesc = {};
			blendDesc.AlphaToCoverageEnable = false;
			blendDesc.IndependentBlendEnable = false;
			blendDesc.RenderTarget[0].BlendEnable = false;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, copyBlendState.put()));
		}

		if (!copyRasterizerState) {
			D3D11_RASTERIZER_DESC rasterizerDesc = {};
			rasterizerDesc.FillMode = D3D11_FILL_SOLID;
			rasterizerDesc.CullMode = D3D11_CULL_NONE;
			rasterizerDesc.FrontCounterClockwise = false;
			rasterizerDesc.DepthBias = 0;
			rasterizerDesc.DepthBiasClamp = 0.0f;
			rasterizerDesc.SlopeScaledDepthBias = 0.0f;
			rasterizerDesc.DepthClipEnable = false;
			rasterizerDesc.ScissorEnable = false;
			rasterizerDesc.MultisampleEnable = false;
			rasterizerDesc.AntialiasedLineEnable = false;
			DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, copyRasterizerState.put()));
		}

		if (!depthStencilState) {
			D3D11_DEPTH_STENCIL_DESC dsDesc = {};
			dsDesc.DepthEnable = TRUE;
			dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dsDesc.StencilEnable = false;
			DX::ThrowIfFailed(device->CreateDepthStencilState(&dsDesc, depthStencilState.put()));
		}
	}

	SetupSkyHemisphere();
	SetupWaterFlowMap();
	SetupSharedTextures();
	CompileShaders();
}

void Raytracing::SetupSkyHemisphere()
{
	if (skyHemisphere)
		return;

	auto* device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = SKY_HEMI_SIZE;
	texDesc.Height = SKY_HEMI_SIZE;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, skyHemisphere.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(skyHemisphere.get(), &srvDesc, skyHemisphereSRV.put()));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(skyHemisphere.get(), &uavDesc, skyHemisphereUAV.put()));

	if (!samplerState) {
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, samplerState.put()));
	}

	waterReflections = RE::NiPointer(new RE::TESWaterReflections());
	waterReflections->flags.set(true, RE::TESWaterReflections::Flags::kDirty, RE::TESWaterReflections::Flags::kDynamicCubemap, RE::TESWaterReflections::Flags::kWorldOrigin);
	for (uint32_t i = 0; i < 6; i++) {
		waterReflections->cubeMapSides[i] = RE::TESWaterReflections::CubeMapSide(i, 0.0f);
	}

	logger::info("SetSkyHemisphere");

	creationEngineRaytracing->SetSkyHemisphere(skyHemisphere.get());
}

void Raytracing::SetupWaterFlowMap()
{
	if (waterFlowMap)
		return;

	auto* device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = WATER_FLOWMAP_SIZE;
	texDesc.Height = WATER_FLOWMAP_SIZE;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, waterFlowMap.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(waterFlowMap.get(), &srvDesc, waterFlowMapSRV.put()));

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = texDesc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;
	DX::ThrowIfFailed(device->CreateRenderTargetView(waterFlowMap.get(), &rtvDesc, waterFlowMapRTV.put()));

	logger::info("SetWaterFlowMap");

	creationEngineRaytracing->SetWaterFlowMap(waterFlowMap.get());
}

void Raytracing::SetupSharedTextures()
{
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto* device = globals::d3d::device;
	if (!device)
		return;

	auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainTex.texture)
		return;

	D3D11_TEXTURE2D_DESC mainDesc{};
	mainTex.texture->GetDesc(&mainDesc);

	if (normalRoughnessTexture) {
		D3D11_TEXTURE2D_DESC currentDesc{};
		normalRoughnessTexture->GetDesc(&currentDesc);
		if (currentDesc.Width != mainDesc.Width || currentDesc.Height != mainDesc.Height) {
			normalRoughnessTexture = nullptr;
			normalRoughnessSRV = nullptr;
			normalRoughnessUAV = nullptr;
		}
	}

	if (!normalRoughnessTexture) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = mainDesc.Width;
		texDesc.Height = mainDesc.Height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, normalRoughnessTexture.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(device->CreateShaderResourceView(normalRoughnessTexture.get(), &srvDesc, normalRoughnessSRV.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(normalRoughnessTexture.get(), &uavDesc, normalRoughnessUAV.put()));
	}

	auto* albedoTex = renderer->GetRuntimeData().renderTargets[ALBEDO].texture;
	auto* gnmaoTex = renderer->GetRuntimeData().renderTargets[MASKS2].texture;

	if (!albedoTex || !gnmaoTex) {
		logger::warn("SetSharedTextures: GBuffer render targets not ready (albedo: {}, gnmao: {})", albedoTex != nullptr, gnmaoTex != nullptr);
		return;
	}

	logger::info("SetSharedTextures");

	creationEngineRaytracing->SetSharedTextures(albedoTex, normalRoughnessTexture.get(), gnmaoTex);

	if (creationEngineRaytracing->GetSharedTextures) {
		CreationEngineRaytracing::SharedTexture depth[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
		CreationEngineRaytracing::SharedTexture motionVector[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
		CreationEngineRaytracing::SharedTexture main[CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT]{};
		creationEngineRaytracing->GetSharedTextures(depth, motionVector, main);

		auto setupSharedWrapper = [device](SharedTextureWrapper& wrapper, const CreationEngineRaytracing::SharedTexture& st) {
			wrapper.texture = st;
			wrapper.srv = nullptr;
			if (st.shared) {
				D3D11_TEXTURE2D_DESC desc{};
				st.shared->GetDesc(&desc);
				if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
					D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
					srvDesc.Format = desc.Format;
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Texture2D.MostDetailedMip = 0;
					srvDesc.Texture2D.MipLevels = 1;
					DX::ThrowIfFailed(device->CreateShaderResourceView(st.shared, &srvDesc, wrapper.srv.put()));
				}
			}
		};

		for (uint32_t i = 0; i < CreationEngineRaytracing::MAX_FRAMES_IN_FLIGHT; i++) {
			setupSharedWrapper(sharedDepthTextures[i], depth[i]);
			setupSharedWrapper(sharedMotionVectorTextures[i], motionVector[i]);
			setupSharedWrapper(sharedMainTextures[i], main[i]);

			logger::info("[Raytracing] SharedTexture[{}]: Depth native={}, shared={}, srv={}",
				i, static_cast<void*>(depth[i].native), static_cast<void*>(depth[i].shared), static_cast<void*>(sharedDepthTextures[i].srv.get()));
			logger::info("[Raytracing] SharedTexture[{}]: MotionVector native={}, shared={}, srv={}",
				i, static_cast<void*>(motionVector[i].native), static_cast<void*>(motionVector[i].shared), static_cast<void*>(sharedMotionVectorTextures[i].srv.get()));
			logger::info("[Raytracing] SharedTexture[{}]: Main native={}, shared={}, srv={}",
				i, static_cast<void*>(main[i].native), static_cast<void*>(main[i].shared), static_cast<void*>(sharedMainTextures[i].srv.get()));
		}
	}
}

void Raytracing::CompileShaders()
{
	std::string skyHemiSize = std::to_string(SKY_HEMI_SIZE);
	if (auto* rawPtr = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CubeToHemiCS.hlsl",
			{ { "RESOLUTION", skyHemiSize.c_str() } },
			"cs_5_0"))) {
		cubeToHemiCS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\PTCompositeCS.hlsl",
			{},
			"cs_5_0"))) {
		ptCompositeCS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CopyDepth.hlsl",
			{},
			"vs_5_0",
			"MainVS"))) {
		copyDepthVS.attach(rawPtr);
	}

	if (auto* rawPtr = static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\Raytracing\\CopyDepth.hlsl",
			{},
			"ps_5_0",
			"MainPS"))) {
		copyDepthPS.attach(rawPtr);
	}
}

void Raytracing::SkyCubeToHemi() const
{
	auto* context = globals::d3d::context;

	context->CSSetShader(cubeToHemiCS.get(), nullptr, 0);

	auto reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
	auto* reflectionOcc = globals::features::cloudShadows.loaded && globals::features::cloudShadows.texCubemapCloudOccCopy ?
	                          globals::features::cloudShadows.texCubemapCloudOccCopy->srv.get() :
	                          nullptr;

	ID3D11ShaderResourceView* srvs[] = {
		reflections.SRV,
		reflectionOcc
	};
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	auto* sampler = samplerState.get();
	context->CSSetSamplers(0, 1, &sampler);

	ID3D11UnorderedAccessView* uav = skyHemisphereUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	uint32_t dispatch = static_cast<uint32_t>(std::ceil(SKY_HEMI_SIZE / 8.0f));
	context->Dispatch(dispatch, dispatch, 1);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11ShaderResourceView* nullSRVs[ARRAYSIZE(srvs)] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), nullSRVs);
}

void Raytracing::CopyWaterFlowMap() const
{
	if (!waterFlowMap || !waterFlowMapRTV)
		return;

	auto* context = globals::d3d::context;

	auto clearFlowMap = [&]() {
		const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(waterFlowMapRTV.get(), clearColor);
	};

	if (REL::Module::IsVR()) {
		clearFlowMap();
		return;
	}

	REL::Relocation<RE::NiPointer<RE::NiSourceTexture>*> gFlowMapSourceTex{ REL::RelocationID(527694, 414616) };
	auto* flowMapSourceTex = gFlowMapSourceTex.get();
	if (!flowMapSourceTex) {
		clearFlowMap();
		return;
	}

	auto* sourceTexture = flowMapSourceTex->get();
	if (!sourceTexture || !sourceTexture->rendererTexture || !sourceTexture->rendererTexture->texture) {
		clearFlowMap();
		return;
	}

	context->CopyResource(waterFlowMap.get(), sourceTexture->rendererTexture->texture);
}

void Raytracing::DrawSettings()
{
	if (disableReason == DisableReason::MissingPlugin) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_missing_plugin"), "Missing 'CreationEngineRaytracing.dll', check your mod manager."));
		return;
	} else if (disableReason == DisableReason::InitFailed) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_init_failed"), "Initialization Failed, check CreationEngineRaytracing.txt log"));
		return;
	}

	ImGui::Text("%s", T(TKEY("status_initialized"), "CreationEngineRaytracing runtime is active and initialized."));
	ImGui::Spacing();

	auto ceRTSettingsBefore = GetSettings();

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.CreationEngineRaytracingSettings.Enabled);

	ImGui::Checkbox(T(TKEY("validation_layer"), "Validation Layer"), &settings.RendererSettings.ValidationLayer);

	const char* modeStr = "None";
	switch (Mode()) {
	case CreationEngineRaytracing::Mode::GlobalIllumination:
		modeStr = "Global Illumination";
		break;
	case CreationEngineRaytracing::Mode::PathTracing:
		modeStr = "Path Tracing";
		break;
	case CreationEngineRaytracing::Mode::Debug:
		modeStr = "Debug";
		break;
	default:
		break;
	}
	ImGui::Text("%s: %s", T(TKEY("active_mode"), "Active Mode"), modeStr);

	if (ceRTSettingsBefore != GetSettings()) {
		UpdateSettings();
	}
}
