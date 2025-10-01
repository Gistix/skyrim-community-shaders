#include "VoxelConeTracingGI/Light.hlsli"
#include "VoxelConeTracingGI/Octree.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

cbuffer InjectLightingCB : register(b0)
{
	float3 VoxelSize;
	uint LightCount;
	float3 Coord2Cell;
	uint _pad0;
	float3 Min;
	uint _pad1;
}

StructuredBuffer<Light> Lights : register(t0);
StructuredBuffer<Voxel> Voxels : register(t1);
StructuredBuffer<Node> Octree : register(t2);

RWTexture3D<half4> LOD0 : register(u0);

[numthreads(64, 1, 1)]
void main(int id: SV_DispatchThreadID)
{
	Voxel voxel = Voxels[id];

	float3 voxelPosition = voxel.position;
	float3 voxelSurface = voxelPosition + (voxel.normal * VoxelSize * 0.5);

	float3 diffuseLighting = 0;

	[loop] // Would a unroled loop to MAX_LIGHTS be faster?
    for (uint i = 0; i < LightCount; ++i) {
		Light light = Lights[i];

		float3 lvector = light.lvector;
		float range = light.range;
		float3 color = light.color;
		uint type = light.type;

		float3 direction;
		float attenuation = 1.0;

		if (type == 0) // Directional
		{
			direction = lvector;
		}
		else if (type == 1) // Point
		{
			float3 vlVector = voxelPosition - lvector;

			float distanceSqr = dot(vlVector, vlVector);

			float distance = sqrt(distanceSqr);

			attenuation = 1.0 / max(distanceSqr, 0.01);

			float fade = saturate(1.0 - pow(distance / range, 4.0));
			attenuation *= fade * fade;

			direction = -(vlVector/distance);
		}
		else // Spot?
		{
			direction = lvector;
		}

		float NdotL = max(dot(voxel.normal, direction), 0);
		diffuseLighting += color * NdotL * attenuation;
	}

	uint3 uvw = (uint3)floor((voxelPosition - Min) * Coord2Cell);
	LOD0[uvw] = half4(voxel.albedo, 1.0);
}