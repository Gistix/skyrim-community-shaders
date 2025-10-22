#ifndef __VOXELCONETRACINGGI_DEPENDENCY_HLSL__
#define __VOXELCONETRACINGGI_DEPENDENCY_HLSL__

#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#define VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED

#define M_TO_GAME_UNIT (1.0 / (GAME_UNIT_TO_M))
#define M_TO_GAME_UNITS_SQ ((M_TO_GAME_UNIT) * (M_TO_GAME_UNIT))

namespace VoxelConeTracingGI
{
    struct Clipmap 
	{
		float3 Min;
		float Size;
		float3 Max;
		float SizeRpc;
	};
}

bool IntersectAABB(float3 boxAMin, float3 boxAMax, float3 boxBMin, float3 boxBMax)
{
    return all(boxAMax >= boxBMin) && all(boxBMax >= boxAMin);
}

#define FP_Z (16.5)

float3 ScreenToViewPosition(const float2 screenPos, const float viewspaceDepth, const float4 ndcToView)
{
	float3 ret;
	ret.xy = (ndcToView.xy * screenPos.xy + ndcToView.zw) * viewspaceDepth;
	ret.z = viewspaceDepth;
	return ret;
}

float3 ViewToWorldPosition(const float3 pos, const float4x4 invView)
{
	float4 worldpos = mul(invView, float4(pos, 1));
	return worldpos.xyz / worldpos.w;
}

float3 ViewToWorldVector(const float3 vec, const float4x4 invView)
{
	return mul((float3x3)invView, vec);
}

#endif
