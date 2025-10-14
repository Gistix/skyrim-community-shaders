#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"
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
    float4 Position     : SV_POSITION;
    centroid float4 TexCoord0    : TEXCOORD0;
    centroid float4 PositionWS   : POSITION3D;
	centroid float3 NormalWS     : NORMAL;
	centroid float4 Color        : COLOR0;
	
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	nointerpolation float3 AABBMin : AABBMIN;
	nointerpolation float3 AABBMax : AABBMAX;
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED    	
};

struct PS_OUT
{
    float4 Albedo   : SV_Target0;   
    float4 Normal   : SV_Target1;
	float4 Emission : SV_Target2; 
};

RWByteAddressBuffer VoxelMask : register(u3);
AppendStructuredBuffer<uint4> VoxelTrack : register(u4);
RWByteAddressBuffer VoxelCount : register(u5);

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexGlowSampler : register(t6);

SamplerState SampColorSampler : register(s0);
SamplerState SampGlowSampler : register(s6);

PS_OUT main(PS_INPUT input)
{  
	PS_OUT output;
	
	float3 uvw = (input.PositionWS.xyz - SharedData::voxelConeTracingGISettings.Min) * SharedData::voxelConeTracingGISettings.SizeInv;

	[branch]
	if (any(uvw < 0.0f) || any(uvw > 1.0f)) {
		output.Albedo = float4(0.0f, 0.0f, 0.0f, 1.0f);
		output.Emission = float4(0.0f, 0.0f, 0.0f, 1.0f);
		output.Normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
	
		return output;
	}
	
	uint3 coord = (uint3)floor(uvw * SharedData::voxelConeTracingGISettings.Res);
	
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	float3 voxelAABBMin = SharedData::voxelConeTracingGISettings.Min + (coord * SharedData::voxelConeTracingGISettings.VoxelSize);
	float3 voxelAABBMax = voxelAABBMin + SharedData::voxelConeTracingGISettings.VoxelSize.xxx;
	
	if (!IntersectAABB(voxelAABBMin, voxelAABBMax, input.AABBMin, input.AABBMax)) {
		output.Albedo = float4(0.0f, 0.0f, 0.0f, 1.0f);
		output.Emission = float4(0.0f, 0.0f, 0.0f, 1.0f);
		output.Normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
		
		return output;
	}
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED	
	
    float2 uv = input.TexCoord0.xy;

	uint index = coord.x + (coord.y * SharedData::voxelConeTracingGISettings.Res) + (coord.z * SharedData::voxelConeTracingGISettings.Res * SharedData::voxelConeTracingGISettings.Res);

	uint originalValue;
	VoxelCount.InterlockedAdd(index * 4, 1, originalValue); 	
	
    uint wordIndex = index >> 5;
    uint bitIndex  = index & 31;
    uint bitMask = 1 << bitIndex;	
	
    uint originalWord;
    VoxelMask.InterlockedOr(wordIndex, bitMask, originalWord);	
	
	if ((originalWord & bitMask) == 0) {
		VoxelTrack.Append(uint4(coord, 0));
	}
	
    output.Albedo = float4(TexColorSampler.Sample(SampColorSampler, uv).rgb, 1.0f);
    output.Emission = float4(EmitColor * TexGlowSampler.Sample(SampGlowSampler, uv).rgb, 1.0f);
    output.Normal = float4(input.NormalWS.xyz, 1.0f);
	
	return output;
}