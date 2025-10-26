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
    centroid float3 PositionWS      : TEXCOORD2;	
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

cbuffer ClipmapCB : register(b13)
{
    VoxelConeTracingGI::ClipmapConstantBuffer ClipmapData;
};

RasterizerOrderedStructuredBuffer<Voxel> ClipVoxels[2] : register(u0);
RWByteAddressBuffer ClipVoxelMask[2] : register(u2);
RWByteAddressBuffer ClipVoxelMap[2] : register(u4);
RWByteAddressBuffer VoxelCount : register(u6);

void VoxelizeClipmap(in const uint clipArrayIdx, in const uint clipIdx, in const uint res, in const uint3 coord, in Voxel voxelSample) 
{
	uint coord1D = coord.x + (coord.y * res) + (coord.z * res * res);
	
    uint wordIndex = coord1D >> 5;
    uint bitIndex  = coord1D & 31;
    uint bitMask = 1 << bitIndex;	
	
    uint originalWord;
    ClipVoxelMask[clipArrayIdx].InterlockedOr(wordIndex * 4, bitMask, originalWord);	
	
	[branch]
	if ((originalWord & bitMask) == 0) { // Voxel at 'coord1D' not voxelized yet, create new instance
		uint sparseIndex;
		VoxelCount.InterlockedAdd(clipIdx * 4, 1, sparseIndex);
	
		// Lets store the sparse voxel index for this coord
		ClipVoxelMap[clipArrayIdx].Store(coord1D * 4, sparseIndex);
	
		// Set voxel to sample
		ClipVoxels[clipArrayIdx][sparseIndex] = voxelSample;
	} else { // Voxel at 'coord1D' already voxelized, so average with previous value
		uint sparseIndex = ClipVoxelMap[clipArrayIdx].Load(coord1D * 4);

		// Rasterizer Ordered Views guarantees this will be ordered
		Voxel prevVoxel = ClipVoxels[clipArrayIdx][sparseIndex];

		prevVoxel.Albedo = (prevVoxel.Albedo + voxelSample.Albedo) * 0.5f; 
		prevVoxel.Normal = normalize((prevVoxel.Normal + voxelSample.Normal) * 0.5f);
		prevVoxel.Emissive = max(prevVoxel.Emissive, voxelSample.Emissive);
		
		ClipVoxels[clipArrayIdx][sparseIndex] = prevVoxel;	
	}
}

void main(PS_INPUT input)
{
	const VoxelConeTracingGI::ConstantBuffer VCTGI = SharedData::voxelConeTracingGISettings;
	
	const uint res = VCTGI.Res;
	
	[branch]
	if (any(input.Cell < 0.0f) || any(input.Cell > (float)res - 1.0f)) {
		return;
	}
	
	const uint clipIdx = input.ClipmapIdx;
	
	/*[branch]
	if (clipIdx > 0) {
		VoxelConeTracingGI::Clipmap prevClipmap = VCTGI.Clipmaps[clipIdx - 1];
		
		[branch]
		if (all(input.PositionWS >= prevClipmap.Min) && all(input.PositionWS <= prevClipmap.Max)) {
			return;
		}
	}*/

	uint3 coord = floor(input.Cell);
	
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
	if (all(voxelSample.Albedo == 0.0f) && all(voxelSample.Emissive == 0.0f)) {
		return;
	}
	
	uint clipmapArrayIdx = clipIdx - ClipmapData.Start;
	
	// This needs to be hardcoded else we cant access array resources
	[branch]
	if (clipmapArrayIdx == 0) {
		VoxelizeClipmap(0, clipIdx, res, coord, voxelSample);
	} else if (clipmapArrayIdx == 1) {
		VoxelizeClipmap(1, clipIdx, res, coord, voxelSample);
	}
#endif
}