#pragma once

#include <PCH.h>

#include "Features/Raytracing/Core/Instance.h"
#include "Features/Raytracing/Core/Clustering/InstanceCluster.h"
#include "Features/Raytracing/Core/Clustering/BVH.h"
#include "Features/Raytracing/Core/Clustering/SweepAndPrune.h"

struct ClusteringProcess
{
	struct ProcessData
	{
		Instance instance;
		eastl::vector<eastl::pair<Instance, float>> instances;
	};

	static eastl::vector<InstanceCluster> ClusterSAP(eastl::unordered_map<RE::NiAVObject*, Instance> instances)
	{
		eastl::vector<Instance> instanceVector;
		eastl::vector<AABB> aabbs;

		for (auto& [node, instance] : instances) {
			instanceVector.push_back(instance);
			aabbs.push_back(instance.aabb);
		}

		SweepAndPrune sap(aabbs);

		static eastl::vector<ProcessData> processData;
		processData.clear();
		processData.reserve(instances.size());

		eastl::vector<eastl::pair<int, int>> pairs;
		sap.computePairs(pairs);

		for (auto& [a, b] : pairs) {
			auto& instanceA = instanceVector[a];
			auto& instanceB = instanceVector[b];

			instanceA.aabb.Overlap(instanceB.aabb);

			/*float overlap = instanceA.aabb.Overlap(instanceB.aabb);
			instanceData.instances.emplace_back(instances[index], overlap);	*/
		}

		return eastl::vector<InstanceCluster>();
	}

	static eastl::vector<InstanceCluster> ClusterBVH(eastl::unordered_map<RE::NiAVObject*, Instance> instances)
	{
		eastl::vector<Instance> instanceVector;
		eastl::vector<AABB> aabbs;

		for (auto& [node, instance] : instances) {
			instanceVector.push_back(instance);
			aabbs.push_back(instance.aabb);
		}

		BVH bvh(aabbs, 8);

		static eastl::vector<ProcessData> processData;
		processData.clear();
		processData.reserve(instances.size());

		eastl::vector<eastl::pair<int, int>> collisionPairs;
		bvh.getAllPairs(collisionPairs);

		for (auto& [a, b] : collisionPairs) {
			auto& instanceA = instanceVector[a];
			auto& instanceB = instanceVector[b];

			instanceA.aabb.Overlap(instanceB.aabb);

			/*float overlap = instanceA.aabb.Overlap(instanceB.aabb);
			instanceData.instances.emplace_back(instances[index], overlap);	*/
		}

		return eastl::vector<InstanceCluster>();
	}
};