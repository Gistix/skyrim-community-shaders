#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Permutation.hlsli"

struct PS_INPUT
{
    float4 Position				: SV_POSITION;
    centroid float4 TexCoord0	: TEXCOORD0;
	centroid float3 NormalWS	: NORMAL;
	centroid float4 Color		: COLOR0;
	centroid float3 Cell		: TEXCOORD1;
	
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	nointerpolation float3 AABBMin : AABBMIN;
	nointerpolation float3 AABBMax : AABBMAX;
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED    	
};

#if defined(LIGHTING)
#include "VoxelConeTracingGI/Voxelize/VoxelizePS.Lighting.hlsli"
#elif defined(EFFECT)
#include "VoxelConeTracingGI/Voxelize/VoxelizePS.Effect.hlsli"
#endif

AppendStructuredBuffer<Voxel> VoxelSamples : register(u0);

void main(PS_INPUT input)
{
	[branch]
	if (any(input.Cell < 0.0f) || any(input.Cell > (float)SharedData::voxelConeTracingGISettings.Res - 1.0f)) {
		return;
	}
	
	uint3 coord = floor(input.Cell);

#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	float3 voxelAABBMin = SharedData::voxelConeTracingGISettings.Min + (coord * SharedData::voxelConeTracingGISettings.VoxelSize);
	float3 voxelAABBMax = voxelAABBMin + SharedData::voxelConeTracingGISettings.VoxelSize.xxx;
	
	[branch]
	if (!IntersectAABB(voxelAABBMin, voxelAABBMax, input.AABBMin, input.AABBMax)) {
		return;
	}
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED

#if defined(LIGHTING) || defined(EFFECT)
	Voxel voxelSample;
	
	voxelSample.Coord = coord;
	voxelSample.Normal = normalize(input.NormalWS.xyz);		
	
	FillVoxelSample(voxelSample, input);
	
	VoxelSamples.Append(voxelSample);
#endif
}