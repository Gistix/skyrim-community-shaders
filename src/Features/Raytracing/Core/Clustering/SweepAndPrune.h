#pragma once

#include <PCH.h>

#include "Features/Raytracing/Utils.h"

struct SAPEntry
{
	float minX;
	int index;
};

class SweepAndPrune
{
public:
	SweepAndPrune(const eastl::vector<AABB>& objects) :
		objects(objects)
	{
		entries.resize(objects.size());
	}

	void computePairs(eastl::vector<eastl::pair<int, int>>& pairs)
	{
		const int count = (int)objects.size();
		pairs.clear();

		// Build entries
		for (int i = 0; i < count; ++i) {
			entries[i].minX = objects[i].min.x;
			entries[i].index = i;
		}

		// Sort by min.x
		eastl::sort(entries.begin(), entries.end(),
			[](const SAPEntry& a, const SAPEntry& b) {
				return a.minX < b.minX;
			});

		// Sweep
		for (int i = 0; i < count; ++i) {
			const int a = entries[i].index;
			const AABB& A = objects[a];

			for (int j = i + 1; j < count; ++j) {
				const int b = entries[j].index;
				const AABB& B = objects[b];

				// Early exit: no overlap on X
				if (B.min.x > A.max.x)
					break;

				// Test Y & Z
				if (A.max.y < B.min.y || A.min.y > B.max.y)
					continue;
				if (A.max.z < B.min.z || A.min.z > B.max.z)
					continue;

				pairs.push_back({ a, b });
			}
		}
	}

private:
	const eastl::vector<AABB>& objects;
	eastl::vector<SAPEntry> entries;
};