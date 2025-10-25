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

RasterizerOrderedStructuredBuffer<Voxel> ClipVoxels[8] : register(u0);
RWByteAddressBuffer ClipVoxelMask[8] : register(u8);
RWByteAddressBuffer ClipVoxelMap[8] : register(u16);
RWByteAddressBuffer VoxelCount : register(u24);

void VoxelizeClipmap(const uint clipIdx, const uint res, const uint3 coord, Voxel voxelSample) 
{
	uint clipOffset = clipIdx * 4; // 'Struct' size is 4 uints (vertice count, instance count, vertex start, instance start)
	uint countIndex = clipOffset + 1; // Voxel Count (instance count) is the 2nd uint
	
	uint coord1D = coord.x + (coord.y * res) + (coord.z * res * res);
	
    uint wordIndex = coord1D >> 5;
    uint bitIndex  = coord1D & 31;
    uint bitMask = 1 << bitIndex;	
	
    uint originalWord;
    ClipVoxelMask[clipIdx].InterlockedOr(wordIndex * 4, bitMask, originalWord);	
	
	[branch]
	if ((originalWord & bitMask) == 0) { // Voxel at 'coord1D' not voxelized yet, create new instance
		uint sparseIndex;
		VoxelCount.InterlockedAdd(countIndex * 4, 1, sparseIndex);
	
		// Lets store the sparse voxel index for this coord
		ClipVoxelMap[clipIdx].Store(coord1D * 4, sparseIndex);
	
		// Set voxel to sample
		ClipVoxels[clipIdx][sparseIndex] = voxelSample;
	} else { // Voxel at 'coord1D' already voxelized, so average with previous value
		uint sparseIndex = ClipVoxelMap[clipIdx].Load(coord1D * 4);

		// Rasterizer Ordered Views guarantees this will be ordered
		Voxel prevVoxel = ClipVoxels[clipIdx][sparseIndex];

		prevVoxel.Albedo = (prevVoxel.Albedo + voxelSample.Albedo) * 0.5f; 
		prevVoxel.Normal = normalize((prevVoxel.Normal + voxelSample.Normal) * 0.5f);
		prevVoxel.Emissive = max(prevVoxel.Emissive, voxelSample.Emissive);
		
		ClipVoxels[clipIdx][sparseIndex] = prevVoxel;	
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
	
	uint3 coord = floor(input.Cell);
	
	const uint clipIdx = input.ClipmapIdx;
	
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
	if (clipIdx == 0) {
		VoxelizeClipmap(0, res, coord, voxelSample);
	} else if (clipIdx == 1) {
		VoxelizeClipmap(1, res, coord, voxelSample);
	} else if (clipIdx == 2) {
		VoxelizeClipmap(2, res, coord, voxelSample);
	} else if (clipIdx == 3) {
		VoxelizeClipmap(3, res, coord, voxelSample);
	} else if (clipIdx == 4) {
		VoxelizeClipmap(4, res, coord, voxelSample);
	} else if (clipIdx == 5) {
		VoxelizeClipmap(5, res, coord, voxelSample);
	} else if (clipIdx == 6) {
		VoxelizeClipmap(6, res, coord, voxelSample);	
	} else if (clipIdx == 7) {
		VoxelizeClipmap(7, res, coord, voxelSample);
	}
#endif
}