#pragma once

#include <PCH.h>

#include "Features/Raytracing/Core/Instance.h"
#include "Features/Raytracing/Core/InstanceCluster.h"
#include "Features/Raytracing/Core/Clustering/BVH.h"
#include "Features/Raytracing/Core/Clustering/SweepAndPrune.h"

struct ClusteringProcess
{
	struct ProcessData
	{
		Instance instance;
		eastl::vector<eastl::pair<Instance, float>> instances;
	};

	struct DSU
	{
		eastl::vector<int> parent;

		DSU(int n) :
			parent(n)
		{
			for (int i = 0; i < n; ++i)
				parent[i] = i;
		}

		int Find(int x)
		{
			return parent[x] == x ? x : parent[x] = Find(parent[x]);
		}

		void Union(int a, int b)
		{
			a = Find(a);
			b = Find(b);
			if (a != b)
				parent[b] = a;
		}
	};

	static float MergeScoreBLAS(const AABB& a, const AABB& b)
	{
		AABB m = AABB::Merge(a, b);

		float expansion = m.Volume() / (a.Volume() + b.Volume());
		float fill = (a.Volume() + b.Volume()) / m.Volume();

		// Hard rejects
		if (expansion > 1.4f)
			return FLT_MAX;
		if (fill < 0.7f)
			return FLT_MAX;

		// Lower is better
		return expansion;
	}

	struct CandidateMerge
	{
		int a, b;
		float score;
	};

	static bool AspectRatioAcceptable(const AABB& merged, float maxRatio = 10.0f)
	{
		return merged.AspectRatio() <= maxRatio;
	}

	static bool ShouldMergeBLAS(const AABB& a, const AABB& b)
	{
		constexpr auto gapFactor = 0.1f;

		auto kMaxGap = gapFactor * std::min(a.size.Length(), b.size.Length());

		// Cheap reject: no proximity
		if (!a.Intersects(b) && a.GapDistance(b) > kMaxGap)
			return false;

		AABB merged = AABB::Merge(a, b);

		const float expansion = merged.Volume() / (a.Volume() + b.Volume());
		if (expansion > 1.4f)
			return false;

		const float fill = (a.Volume() + b.Volume()) / merged.Volume();
		if (fill < 0.7f)
			return false;

		if (!AspectRatioAcceptable(merged))
			return false;

		return true;
	}

	/*static eastl::vector<InstanceCluster> ClusterSAP(eastl::unordered_map<RE::NiAVObject*, Instance> instances)
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

			bool merge = ShouldMergeBLAS(instanceA.aabb, instanceB.aabb);

			//float overlap = instanceA.aabb.Overlap(instanceB.aabb);
			//instanceData.instances.emplace_back(instances[index], overlap);
		}

		return eastl::vector<InstanceCluster>();
	}*/

	static auto ClusterBVH(eastl::unordered_map<RE::NiAVObject*, Instance> instances)
	{
		eastl::vector<Instance> instanceVector;
		eastl::vector<AABB> aabbs;

		for (auto& [node, instance] : instances) {
			instance.UpdateAABB();
			instanceVector.push_back(instance);
			aabbs.push_back(instance.aabb);
		}

		BVH bvh(aabbs, 8);

		static eastl::vector<ProcessData> processData;
		processData.clear();
		processData.reserve(instances.size());

		eastl::vector<eastl::pair<int, int>> collisionPairs;
		bvh.getAllPairs(collisionPairs);

		eastl::vector<InstanceCluster> instanceClusters;

		for (auto& [a, b] : collisionPairs) {
			auto& instanceA = instanceVector[a];
			auto& instanceB = instanceVector[b];

			bool merge = ShouldMergeBLAS(instanceA.aabb, instanceB.aabb);
			logger::info("Merge: {}", merge);
		}

		return instanceClusters;
	}

	/*static auto ClusterBVH2(eastl::unordered_map<RE::NiAVObject*, Instance> instances)
	{
		// Flatten input
		eastl::vector<RE::NiAVObject*> nodes;
		eastl::vector<AABB> aabbs;

		nodes.reserve(instances.size());
		aabbs.reserve(instances.size());

		for (auto& [node, instance] : instances) {
			nodes.push_back(node);
			aabbs.push_back(instance.aabb);
		}

		const int count = (int)aabbs.size();

		// Build BVH for candidate generation
		BVH bvh(aabbs, 8);

		eastl::vector<eastl::pair<int, int>> collisionPairs;
		bvh.getAllPairs(collisionPairs);

		// Cluster ownership
		DSU dsu(count);

		// Current cluster bounds (one per root)
		eastl::vector<AABB> clusterAABB = aabbs;

		// --- Collect + sort candidate merges ---
		struct Candidate
		{
			int a, b;
			float score;
		};

		eastl::vector<Candidate> candidates;
		candidates.reserve(collisionPairs.size());

		for (auto& [a, b] : collisionPairs) {
			const AABB& A = aabbs[a];
			const AABB& B = aabbs[b];

			if (!ShouldMergeBLAS(A, B))
				continue;

			AABB merged = AABB::Merge(A, B);
			float expansion = merged.Volume() / (A.Volume() + B.Volume());

			candidates.push_back({ a, b, expansion });
		}

		// Best merges first
		eastl::sort(candidates.begin(), candidates.end(),
			[](const Candidate& x, const Candidate& y) {
				return x.score < y.score;
			});

		// --- Apply merges greedily ---
		for (auto& c : candidates) {
			int ra = dsu.Find(c.a);
			int rb = dsu.Find(c.b);

			if (ra == rb)
				continue;

			const AABB& A = clusterAABB[ra];
			const AABB& B = clusterAABB[rb];

			if (!ShouldMergeBLAS(A, B))
				continue;

			AABB merged = AABB::Merge(A, B);

			dsu.Union(ra, rb);
			int root = dsu.Find(ra);
			clusterAABB[root] = merged;
		}

		// --- Emit InstanceClusters ---
		eastl::unordered_map<int, InstanceCluster> clusterMap;

		for (int i = 0; i < count; ++i) {
			int root = dsu.Find(i);
			auto& cluster = clusterMap[root];

			if (cluster.instances.empty())
				cluster.aabb = clusterAABB[root];

			cluster.instances.push_back(nodes[i]);
		}

		eastl::vector<InstanceCluster> result;
		result.reserve(clusterMap.size());

		for (auto& [_, cluster] : clusterMap)
			result.push_back(eastl::move(cluster));

		return result;
	}*/
};