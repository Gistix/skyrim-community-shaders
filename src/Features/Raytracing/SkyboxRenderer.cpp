#include "SkyboxRenderer.h"

void SkyboxRenderer::Initialize()
{
	/*auto* rtManager = RE::BSGraphics::RenderTargetManager::GetSingleton();

	RE::NiPointer<RE::NiRenderTargetGroup> rtGroup;
	rtManager->CreateRenderTarget(
		rtGroup,
		width,
		height,
		RE::BSGraphics::TextureFormat::kRGBA8,
		true  // depth
	);*/

	//camera = RE::NiPointer<RE::NiCamera>(RE::NiCamera::Create());
	auto& runtimeData = camera->GetRuntimeData2();

	runtimeData.viewFrustum.fNear = 0.1f;
	runtimeData.viewFrustum.fFar = 100000.0f;
	runtimeData.viewFrustum.fLeft = -1.0f;
	runtimeData.viewFrustum.fRight = 1.0f;
	runtimeData.viewFrustum.fTop = 1.0f;
	runtimeData.viewFrustum.fBottom = -1.0f;

	camera->world.translate = { 0.f, 0.f, 0.f };
	auto updateData = RE::NiUpdateData(0.0f, RE::NiUpdateData::Flag::kDirty);
	camera->UpdateWorldData(&updateData);
}

void SkyboxRenderer::Render([[maybe_unused]]WrappedResource* skyHemisphere) const
{
	//globals::d3d::context;

	auto* sky = RE::Sky::GetSingleton();
	//auto* renderer = globals::game::renderer;

	//auto* renderer = RE::BSRenderManager::GetSingleton();

	RE::BSMultiBoundNode* skyRoot = sky->root.get();
	skyRoot->CullNode(false);
	//skyRoot->ren

}