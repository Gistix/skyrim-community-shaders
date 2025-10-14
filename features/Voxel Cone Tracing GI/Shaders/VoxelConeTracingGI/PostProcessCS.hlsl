#include "VoxelConeTracingGI/Voxel.hlsli"

cbuffer PostProcessCB : register(b0)
{
	float3 Min;
	float Size;
	float SizeInv;
	uint Res;
	float ResInv;
	float VoxelSize;
}

Texture2D<float4> Albedo			: register(t0);
Texture2D<float4> Normal			: register(t1);
Texture2D<float4> Emissive			: register(t2);
StructuredBuffer<uint4> VoxelTrack	: register(t3);
ByteAddressBuffer SampleCount		: register(t4);

RWStructuredBuffer<Voxel> Voxels : register(u0);

[numthreads(64, 1, 1)]
void main(uint id: SV_DispatchThreadID, uint groupId: SV_GroupThreadID)
{
	uint3 coord = VoxelTrack[id].xyz;

	uint index = coord.x + (coord.y * Res) + (coord.z * Res * Res);
	
	uint2 coord2 = uint2(coord.x + coord.z * Res, coord.y);
	
	uint samples = SampleCount.Load(index * 4);
		
	Voxel voxel;
	
	voxel.Coord = coord;
	voxel.Albedo = Albedo[coord2].rgb / samples;
	voxel.Normal = Normal[coord2].rgb / samples;
	voxel.Emissive = Emissive[coord2].rgb;
	
	Voxels[id] = voxel;
}