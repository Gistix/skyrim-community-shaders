#include "Common/SharedData.hlsli"
#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

#include "Common/Color.hlsli"
#include "Common/Permutation.hlsli"

struct PS_INPUT
{
    float4 Position					: SV_POSITION;
    centroid float4 TexCoord0		: TEXCOORD0;
	centroid float3 NormalWS		: NORMAL;
	centroid float4 Color			: COLOR0;
	centroid float3 Cell			: TEXCOORD1;
	nointerpolation uint ClipmapIdx : INSTANCE_ID;
		
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

/*#ifndef CLIPMAP_COUNT
#define CLIPMAP_COUNT 4
#endif*/

AppendStructuredBuffer<Voxel> Clip0Samples : register(u0);
AppendStructuredBuffer<Voxel> Clip1Samples : register(u1);
AppendStructuredBuffer<Voxel> Clip2Samples : register(u2);
AppendStructuredBuffer<Voxel> Clip3Samples : register(u3);
AppendStructuredBuffer<Voxel> Clip4Samples : register(u4);
AppendStructuredBuffer<Voxel> Clip5Samples : register(u5);
AppendStructuredBuffer<Voxel> Clip6Samples : register(u6);
AppendStructuredBuffer<Voxel> Clip7Samples : register(u7);

/*#if CLIPMAP_COUNT > 1
AppendStructuredBuffer<Voxel> Clip1Samples : register(u1);
#endif

#if CLIPMAP_COUNT > 2
AppendStructuredBuffer<Voxel> Clip2Samples : register(u2);
#endif

#if CLIPMAP_COUNT > 3
AppendStructuredBuffer<Voxel> Clip3Samples : register(u3);
#endif

#if CLIPMAP_COUNT > 4
AppendStructuredBuffer<Voxel> Clip4Samples : register(u4);
#endif

#if CLIPMAP_COUNT > 5
AppendStructuredBuffer<Voxel> Clip5Samples : register(u5);
#endif

#if CLIPMAP_COUNT > 6
AppendStructuredBuffer<Voxel> Clip6Samples : register(u6);
#endif

#if CLIPMAP_COUNT > 7
AppendStructuredBuffer<Voxel> Clip7Samples : register(u7);
#endif*/


void main(PS_INPUT input)
{
	const VoxelConeTracingGI::ConstantBuffer VCTGI = SharedData::voxelConeTracingGISettings;
	
	[branch]
	if (any(input.Cell < 0.0f) || any(input.Cell > (float)VCTGI.Res - 1.0f)) {
		return;
	}
	
	uint3 coord = floor(input.Cell);
	
	uint clipIdx = input.ClipmapIdx;
	
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
    VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[clipIdx];
	float voxelSize = (clipmap.Size * VCTGI.ResRcp) * RCP_SCALE;
		
	float3 voxelAABBMin = clipmap.Min + (coord * voxelSize);
	float3 voxelAABBMax = voxelAABBMin + voxelSize;
	
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
	
	
	[branch] 
	switch(clipIdx)
	{
		case 0:
			Clip0Samples.Append(voxelSample);
			break;
		case 1:
			Clip1Samples.Append(voxelSample);
			break;
		case 2:
			Clip2Samples.Append(voxelSample);
			break;
		case 3:
			Clip3Samples.Append(voxelSample);
			break;
		case 4:
			Clip4Samples.Append(voxelSample);
			break;
		case 5:
			Clip5Samples.Append(voxelSample);
			break;
		case 6:
			Clip6Samples.Append(voxelSample);
			break;
		case 7:
			Clip7Samples.Append(voxelSample);
			break;
		default:
			break;
	}

#endif
}