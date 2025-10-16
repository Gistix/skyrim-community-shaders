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

Texture2D<half4> Albedo			: register(t0);
Texture2D<half4> Normal			: register(t1);
Texture2D<half4> Emissive			: register(t2);
StructuredBuffer<uint4> VoxelTrack	: register(t3);
ByteAddressBuffer SampleCount		: register(t4);

RWStructuredBuffer<Voxel> Voxels : register(u0);
RWTexture3D<half4> LOD0 : register(u1);

[numthreads(8, 8, 8)]
void main(uint3 id: SV_DispatchThreadID)
{
	//uint index = id.x + (id.y * Res) + (id.z * Res * Res);
	uint2 coord = uint2(id.x + id.z * Res, id.y);
	
	float4 albedo = Albedo[coord];
	albedo.rgb /= albedo.a;
		
	float4 normal = Normal[coord];
	normal.xyz = normalize(normal.xyz / normal.w);
		
	float3 emissive = Emissive[coord].rgb;	
	
	LOD0[id] = half4((saturate(dot(float3(0, 1, 0), normal.xyz)) * albedo.rgb) + emissive, 1.0f);
	
	/*uint4 voxelTrack = VoxelTrack[id];

	uint3 coord = voxelTrack.xyz;
	uint index = voxelTrack.w; // index = coord.x + (coord.y * Res) + (coord.z * Res * Res)
	
	uint samples = SampleCount.Load(index * 4);
		
	[branch]
	if (samples > 0) {	
		uint2 coord2D = uint2(coord.x + coord.z * Res, coord.y);
		
		float3 albedo = Albedo[coord2D].rgb / samples;		
		float3 normal = normalize(Normal[coord2D].rgb / samples);
		float3 emissive = Emissive[coord2D].rgb;
				
		Voxel voxel;
	
		voxel.Coord = coord;
		voxel.Albedo = albedo.rgb;
		voxel.Normal = normal.xyz;
		voxel.Emissive = emissive;
	
		Voxels[index] = voxel;
		
		LOD0[coord] = half4((saturate(dot(float3(0, 1, 0), normal.xyz)) * albedo.rgb) + emissive, 1.0f);
	}*/
}