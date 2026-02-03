#include "Instance.h"

#include "Features/Raytracing.h"
#include "Features/Raytracing/RE/AABB.h"

void Instance::SetDetached(bool detach)
{
	detached = detach;
}

bool Instance::IsDetached() const
{
	return detached;
}

bool Instance::ShouldUpdate(RE::NiAVObject* node, RE::NiPoint3 cameraPosition)
{
	bool vur = globals::features::raytracing.settings.AdvancedSettings.VariableUpdateRate;

	uint delta = globals::state->frameCount - lastUpdate;

	if (!vur && delta > 0) {
		lastUpdate = globals::state->frameCount;
		return true;
	}

	float distance = Util::Units::GameUnitsToMeters(node->world.translate.GetDistance(cameraPosition));

	if (delta >= UpdateRate(distance)) {
		lastUpdate = globals::state->frameCount;
		return true;
	}

	return false;
}

void Instance::UpdateAABB()
{
	aabb = AABB(Float3(root->worldBound.center), float3::One * root->worldBound.radius * 2.0f);
}

/*void Instance::UpdateAABB()
{
	auto dataName = RE::AABB::DataName();
	auto extraData = Raytracing::GetExtraDataByName(root, &dataName);

	if (extraData) {
		auto aabbData = reinterpret_cast<RE::AABB*>(extraData);

		auto world = GetXMFromNiTransform(root->world);

		auto center = Float3(aabbData->center);
		auto extents = Float3(aabbData->extents);

		DirectX::XMVECTOR centerLS = XMLoadFloat3(&center);
		DirectX::XMVECTOR centerWS = XMVector3TransformCoord(centerLS, world);

		DirectX::XMVECTOR extentsLS = XMLoadFloat3(&extents);

		DirectX::XMVECTOR r0 = DirectX::XMVectorAbs(world.r[0]);
		DirectX::XMVECTOR r1 = DirectX::XMVectorAbs(world.r[1]);
		DirectX::XMVECTOR r2 = DirectX::XMVectorAbs(world.r[2]);

		DirectX::XMVECTOR worldExtents =
			DirectX::XMVectorAdd(
				DirectX::XMVectorAdd(
					DirectX::XMVectorMultiply(r0, DirectX::XMVectorSplatX(extentsLS)),
					DirectX::XMVectorMultiply(r1, DirectX::XMVectorSplatY(extentsLS))),
				DirectX::XMVectorMultiply(r2, DirectX::XMVectorSplatZ(extentsLS)));

		DirectX::XMFLOAT3 min, max;
		XMStoreFloat3(&min, DirectX::XMVectorSubtract(centerWS, worldExtents));
		XMStoreFloat3(&max, DirectX::XMVectorAdd(centerWS, worldExtents));

		aabb = AABB::FromMinMax(min, max);
	}
}*/

// Checks for skinned and dynamic trishapes update
void Instance::Update(RE::NiAVObject* node, RE::NiPoint3 cameraPosition, const eastl::pair<eastl::string, Model*>& modelPair, SkinningPipeline* skinningPipeline)
{
	// Instance was not changed by the game, so there is no need to update it
	// This doesn't work at all for actors
	/*if (pNiNode->lastUpdatedFrameCounter < globals::state->frameCount && hasUpdated)
			return true;*/

	// Instance has already been updated this frame
	if (!ShouldUpdate(node, cameraPosition))
		return;

	// Sets the BLAS instance transform
	XMStoreFloat3x4(&transform, GetXMFromNiTransform(node->world));

	auto dataName = RE::AABB::DataName();
	auto extraData = Raytracing::GetExtraDataByName(node, &dataName);

	if (extraData) {
		auto aabbData = reinterpret_cast<RE::AABB*>(extraData);

		auto min = Float3(node->world * (aabbData->center - aabbData->extents));
		auto max = Float3(node->world * (aabbData->center + aabbData->extents));

		aabb = AABB::FromMinMax(min, max);

		//logger::info("AABB Center: {} Size: {}, Center: {}, Radius: {}", aabb.center, aabb.extents, node->worldBound.center, node->worldBound.radius);
	}

	/*if (node->GetAppCulled())
		return;*/

	auto& [path, model] = modelPair;

	bool isRenderUseValid = model->IsRenderUseValid();

	for (auto& shape : model->shapes) {
		if (isRenderUseValid && shape->geometry->GetFlags().none(RE::NiAVObject::Flag::kRenderUse)) {
			shape->state |= Shape::State::Hidden;
			continue;
		} else {
			shape->state &= ~Shape::State::Hidden;
		}

		if ((model->GetFlags() & Shape::Flags::Dynamic) || (model->GetFlags() & Shape::Flags::Skinned)) {
			Shape::Flags updateFlags = Shape::Flags::None;

			if (shape->UpdateDynamicPosition()) {
				updateFlags |= Shape::Flags::Dynamic;
			}

			if (shape->UpdateSkinning()) {
				updateFlags |= Shape::Flags::Skinned;
			}

			if (updateFlags & Shape::Flags::Skinned) {
				auto& skinInstance = shape->geometry->GetGeometryRuntimeData().skinInstance;

				if (shape->boneMatrices.empty())
					shape->boneMatrices.resize(skinInstance->numMatrices);

				float3x4* boneMatricesArray = reinterpret_cast<float3x4*>(skinInstance->boneMatrices);

				auto rootParent = skinInstance->rootParent;
				auto skinRootInverse = GetXMFromNiTransform(rootParent->world.Invert());

				shape->boundRadius = rootParent->worldBound.radius + (rootParent->world.translate + rootParent->worldBound.center).GetDistance(shape->geometry->world.translate);

				for (uint i = 0; i < skinInstance->numMatrices; i++) {
					XMStoreFloat3x4(&shape->boneMatrices[i], XMMatrixMultiply(XMLoadFloat3x4(&boneMatricesArray[i]), skinRootInverse));
				}
			}

			if ((updateFlags & Shape::Flags::Dynamic) || (updateFlags & Shape::Flags::Skinned)) {
				skinningPipeline->QueueUpdate(updateFlags, path, shape.get());
			}
		}
	}
}