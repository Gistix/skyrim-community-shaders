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
Texture2D<half4> Emissive		: register(t2);

AppendStructuredBuffer<Voxel> Voxels : register(u0);
RWTexture3D<half4> LOD0				 : register(u1);

[numthreads(8, 8, 8)]
void main(uint3 id: SV_DispatchThreadID)
{
	//uint index = id.x + (id.y * Res) + (id.z * Res * Res);
	//uint samples = SampleCount.Load(index * 4);
	uint2 coord = uint2(id.x + id.z * Res, id.y);
	
	float4 albedo = Albedo[coord];
	
	[branch]
	if (albedo.a > 0) {
		albedo.rgb /= albedo.a;
		
		float4 normal = Normal[coord];
		normal.xyz = normalize(normal.xyz / normal.w);
		
		float3 emissive = Emissive[coord].rgb;	
	
		LOD0[id] = half4(albedo.rgb, 1.0f);
	
		Voxel voxel;
	
		voxel.Coord = id;
		voxel.Albedo = albedo.rgb;
		voxel.Normal = normal.xyz;
		voxel.Emissive = emissive;
	
		Voxels.Append(voxel);
	}
	
	//LOD0[id] = half4((saturate(dot(float3(0, 1, 0), normal.xyz)) * albedo.rgb) + emissive, 1.0f);
}