#pragma once

#include <PCH.h>

#include "Features/Raytracing/Utils.h"

struct BVHNode
{
	AABB bounds;
	int left = -1;
	int right = -1;
	int start = -1;  // start index in leaf
	int count = 0;   // number of objects in leaf
};

class BVH
{
public:
	BVH(const eastl::vector<AABB>& objects, int maxLeafSize = 4) :
		objects(objects), maxLeafSize(maxLeafSize)
	{
		indices.resize(objects.size());
		for (int i = 0; i < (int)indices.size(); ++i)
			indices[i] = i;

		nodes.reserve(objects.size() * 2);
		root = build(0, (int)indices.size());
	}

	// BVH vs BVH pairwise query
	void getAllPairs(eastl::vector<eastl::pair<int, int>>& pairs) const
	{
		if (root < 0)
			return;

		struct NodePair
		{
			int a, b;
		};
		eastl::vector<NodePair> stack;
		stack.reserve(1024);

		// Start with root vs root
		stack.push_back({ root, root });

		while (!stack.empty()) {
			NodePair p = stack.back();
			stack.pop_back();

			const BVHNode& A = nodes[p.a];
			const BVHNode& B = nodes[p.b];

			// Prune
			if (!A.bounds.IntersectsSIMD(B.bounds))
				continue;

			// Leaf vs leaf -> generate object pairs
			if (A.count > 0 && B.count > 0) {
				for (int i = 0; i < A.count; ++i) {
					int ia = indices[A.start + i];
					for (int j = 0; j < B.count; ++j) {
						int ib = indices[B.start + j];

						// avoid duplicates + self
						if (ia >= ib)
							continue;

						if (objects[ia].IntersectsSIMD(objects[ib]))
							pairs.push_back({ ia, ib });
					}
				}
				continue;
			}

			// SAME NODE: descend only once (CRITICAL)
			if (p.a == p.b) {
				if (A.left >= 0 && A.right >= 0) {
					stack.push_back({ A.left, A.left });
					stack.push_back({ A.left, A.right });
					stack.push_back({ A.right, A.right });
				}
				continue;
			}

			// One leaf, one internal
			if (A.count > 0) {
				stack.push_back({ p.a, B.left });
				stack.push_back({ p.a, B.right });
			} else if (B.count > 0) {
				stack.push_back({ A.left, p.b });
				stack.push_back({ A.right, p.b });
			} else {
				// Two different internal nodes
				stack.push_back({ A.left, B.left });
				stack.push_back({ A.left, B.right });
				stack.push_back({ A.right, B.left });
				stack.push_back({ A.right, B.right });
			}
		}
	}

private:
	const eastl::vector<AABB>& objects;
	eastl::vector<int> indices;
	eastl::vector<BVHNode> nodes;
	int root = -1;
	int maxLeafSize = 4;

	int build(int start, int end)
	{
		int count = end - start;
		if (count <= 0)
			return -1;

		BVHNode node;
		node.bounds = objects[indices[start]];
		for (int i = start + 1; i < end; ++i)
			node.bounds = AABB::Merge(node.bounds, objects[indices[i]]);

		int nodeIndex = (int)nodes.size();
		nodes.push_back(node);

		// Leaf condition
		if (count <= maxLeafSize) {
			node.start = start;
			node.count = count;
			nodes[nodeIndex] = node;
			return nodeIndex;
		}

		// Split axis (longest)
		float3 extent = node.bounds.size;
		int axis = 0;
		if (extent.y > extent.x)
			axis = 1;
		if ((axis == 0 ? extent.z : axis == 1 ? extent.z > extent.y :
												extent.z > extent.x))
			axis = 2;

		// Fallback: all centers identical
		float minVal = axis == 0 ? node.bounds.min.x :
		               axis == 1 ? node.bounds.min.y :
		                           node.bounds.min.z;
		float maxVal = axis == 0 ? node.bounds.max.x :
		               axis == 1 ? node.bounds.max.y :
		                           node.bounds.max.z;
		if (maxVal - minVal < 1e-6f) {
			node.start = start;
			node.count = count;
			nodes[nodeIndex] = node;
			return nodeIndex;
		}

		// Partition along axis
		int mid = start + count / 2;
		if (mid == start)
			mid = start + 1;
		if (mid == end)
			mid = end - 1;

		eastl::nth_element(
			indices.begin() + start,
			indices.begin() + mid,
			indices.begin() + end,
			[&](int a, int b) {
				float valA = axis == 0 ? objects[a].center.x :
			                 axis == 1 ? objects[a].center.y :
			                             objects[a].center.z;
				float valB = axis == 0 ? objects[b].center.x :
			                 axis == 1 ? objects[b].center.y :
			                             objects[b].center.z;
				return valA < valB;
			});

		node.left = build(start, mid);
		node.right = build(mid, end);
		nodes[nodeIndex] = node;

		return nodeIndex;
	}
};