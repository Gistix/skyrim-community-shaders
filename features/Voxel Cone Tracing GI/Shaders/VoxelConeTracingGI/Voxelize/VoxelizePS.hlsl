#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"
#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
	float4 ShadowLightMaskSelect : packoffset(c2);
	float4 MaterialData : packoffset(c3);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c4);
	float3 EmitColor : packoffset(c4.y);
	float4 ProjectedUVParams : packoffset(c6);
	float4 SSRParams : packoffset(c7);
	float4 WorldMapOverlayParametersPS : packoffset(c8);
	float4 ProjectedUVParams2 : packoffset(c9);
	float4 ProjectedUVParams3 : packoffset(c10);  // fProjectedUVDiffuseNormalTilingScale in x, fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c11);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c14);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[7] : packoffset(c15);               // point light radius in w
	float4 PointLightColor[7] : packoffset(c22);
	float2 NumLightNumShadowLight : packoffset(c29);
#	else
	// VR is [49] instead of [30]
	float3 DirLightDirection : packoffset(c0);
	float4 UnknownPerGeometry[12] : packoffset(c1);
	float3 DirLightColor : packoffset(c13);
	float4 ShadowLightMaskSelect : packoffset(c14);
	float4 MaterialData : packoffset(c15);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c16);
	float3 EmitColor : packoffset(c16.y);
	float4 ProjectedUVParams : packoffset(c18);
	float4 SSRParams : packoffset(c19);
	float4 WorldMapOverlayParametersPS : packoffset(c20);
	float4 ProjectedUVParams2 : packoffset(c21);
	float4 ProjectedUVParams3 : packoffset(c22);  // fProjectedUVDiffuseNormalTilingScale in x,	fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c23);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c26);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[14] : packoffset(c27);              // point light radius in w
	float4 PointLightColor[7] : packoffset(c41);
	float2 NumLightNumShadowLight : packoffset(c48);
#	endif  // VR
};

struct PS_INPUT
{
    float4 Position						: SV_POSITION;
    centroid float4 TexCoord0			: TEXCOORD0;
	centroid float3 NormalWS			: NORMAL;
	centroid float4 Color				: COLOR0;
	centroid float3 Cell				: TEXCOORD1;
	
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	nointerpolation float3 AABBMin : AABBMIN;
	nointerpolation float3 AABBMax : AABBMAX;
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED    	
};

AppendStructuredBuffer<Voxel> VoxelSamples : register(u0);

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexGlowSampler : register(t6);

SamplerState SampColorSampler : register(s0);
SamplerState SampGlowSampler : register(s6);

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

	float2 uv = input.TexCoord0.xy;

	Voxel voxelSample;
	
	voxelSample.Coord = coord;
	voxelSample.Albedo = Color::Diffuse(TexColorSampler.Sample(SampColorSampler, uv).rgb);
	voxelSample.Normal = normalize(input.NormalWS.xyz);
	voxelSample.Emissive = EmitColor * Color::Diffuse(TexGlowSampler.Sample(SampGlowSampler, uv).rgb);
	
	VoxelSamples.Append(voxelSample);
}