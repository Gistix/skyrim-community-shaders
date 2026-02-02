#pragma once

#include "PCH.h"

#include <d3d12.h>
#include <D3D12MemAlloc.h>

#include "Features/Raytracing/Buffer.h"
#include "Features/Raytracing/Types.h"

#include "Features/Raytracing/Core/Shape.h"
#include "Features/Raytracing/Core/Model.h"
#include "Features/Raytracing/Core/Instance.h"

struct ClusterAction
{
	bool Added;
	RE::NiAVObject* Instance;
};

struct InstanceCluster
{
	eastl::vector<RE::NiAVObject*> instances;
	//eastl::hash_set<RE::NiAVObject*> instancesMap;

	winrt::com_ptr<D3D12MA::Allocation> blas = nullptr;
	winrt::com_ptr<D3D12MA::Allocation> scratch = nullptr;

	float3x4 transform;

	eastl::unordered_map<eastl::string, eastl::unique_ptr<Model>>& Models() const;
	eastl::unordered_map<RE::NiAVObject*, Instance>& Instances() const;

	InstanceCluster()
	{
		transform = float3x4(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f);
	}

	template <typename Func>
	void ForEachModel(Func&& func)
	{
		for (auto& root : instances) {
			// There are a lot of layers..
			auto instanceIt = Instances().find(root);
			if (instanceIt == Instances().end())
				continue;

			auto& instance = instanceIt->second;

			auto modelIt = Models().find(instance.filename);
			if (modelIt == Models().end())
				continue;

			func(modelIt->second.get());
		}
	}

	template <typename Func>
	void ForEachShape(Func&& func)
	{
		ForEachModel([&](Model* model) {
			for (auto& shape : model->shapes) {
				func(shape.get());
			}
		});
	}

	void Commit(ID3D12GraphicsCommandList4* commandList);
	void Update(ID3D12GraphicsCommandList4* commandList);
};
