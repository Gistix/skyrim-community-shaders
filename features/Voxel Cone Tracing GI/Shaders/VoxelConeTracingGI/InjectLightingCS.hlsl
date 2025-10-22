#include "Common/Game.hlsli"
#include "Common/Color.hlsli"

#include "VoxelConeTracingGI/Light.hlsli"
#include "VoxelConeTracingGI/Octree.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

cbuffer InjectLightingCB : register(b0)
{
	float3 Min;
	float Size;
	float ResInv; 
	float VoxelSize;
	uint LightCount;
	float EmissiveMult;
	float AmbientMult;
	uint Pad0;
	uint Pad1;
	uint Pad2;	
}

StructuredBuffer<Light> Lights : register(t0);
StructuredBuffer<Voxel> Voxels : register(t1);
StructuredBuffer<Node> Octree : register(t2);

RWTexture3D<half4> LOD0 : register(u0);

[numthreads(64, 1, 1)]
void main(uint id: SV_DispatchThreadID, uint groupId: SV_GroupThreadID)
{
	Voxel voxel = Voxels[id];

	uint3 coord = voxel.Coord;
	
	float3 position = Min + ((coord + 0.5.xxx) * Size * ResInv);
	float3 normal = voxel.Normal;
	
	float3 voxelSurface = position + (normal * VoxelSize * 0.5);

	float3 diffuseLighting = 0;

	[loop] // Would a unroled loop to MAX_LIGHTS be faster?
    for (uint i = 0; i < LightCount; ++i) {
		Light light = Lights[i];

		float3 lvector = light.lvector;
		float range = light.range;
		float3 color = Color::GammaToLinear(light.color);
		uint type = light.type;

		float3 direction;
		float attenuation = 1.0;

		if (type == 0) // Directional
		{
			direction = lvector;
		}
		else if (type == 1) // Point
		{
			float3 vlVector = (lvector - position) * (GAME_UNIT_TO_M);

			float distanceSqr = dot(vlVector, vlVector);

			float distance = sqrt(distanceSqr);

			attenuation = 1.0 / max(distanceSqr, 0.01);

			float fade = saturate(1.0 - pow(distance / range, 4.0));
			attenuation *= fade * fade;

			direction = vlVector/distance;
		}
		else // Spot?
		{
			direction = lvector;
		}

		float NdotL = dot(normal, normalize(direction));
		diffuseLighting += color * saturate(NdotL) * attenuation;
	}

	float3 diffuseColor = voxel.Albedo * diffuseLighting;
	float3 ambientColor = Color::GammaToLinear(max(0, mul(SharedData::DirectionalAmbient, float4(normal, 1.0)))) * AmbientMult;	
	float3 emissiveColor = voxel.Emissive * EmissiveMult;
	
	float3 finalColor = diffuseColor + ambientColor + emissiveColor;

	LOD0[coord] = half4(finalColor, 1.0); // Alpha is used to test visibility during cone tracing
}