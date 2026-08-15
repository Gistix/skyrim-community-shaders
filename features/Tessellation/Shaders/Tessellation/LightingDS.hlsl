#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/LightingCommon.hlsli"

// Mirrors of the game's bound buffers (see Tessellation::MirrorParallaxBindings).
// Partial layouts: only the members this shader reads are declared, with
// matching packoffsets so the runtime maps them correctly.
cbuffer PerMaterial : register(b1)
{
    float4 LODTexParams : packoffset(c0); // TerrainTexOffset in xy, LodBlendingEnabled in z
#if !(defined(LANDSCAPE) && defined(TRUE_PBR))
    float4 TintColor : packoffset(c1);
    float4 EnvmapData : packoffset(c2); // fEnvmapScale in x, 1 or 0 in y depending of if has envmask
    float4 ParallaxOccData : packoffset(c3);
    float4 SpecularColor : packoffset(c4); // Shininess in w, color in xyz
    float4 SparkleParams : packoffset(c5);
    float4 MultiLayerParallaxData : packoffset(c6); // Layer thickness in x, refraction scale in y, uv scale in zw
#else
	float4 LandscapeTexture1GlintParameters : packoffset(c1);
	float4 LandscapeTexture2GlintParameters : packoffset(c2);
	float4 LandscapeTexture3GlintParameters : packoffset(c3);
	float4 LandscapeTexture4GlintParameters : packoffset(c4);
	float4 LandscapeTexture5GlintParameters : packoffset(c5);
	float4 LandscapeTexture6GlintParameters : packoffset(c6);
#endif
    float4 LightingEffectParams : packoffset(c7); // fSubSurfaceLightRolloff in x, fRimLightPower in y
    float4 IBLParams : packoffset(c8);

#if !defined(TRUE_PBR)
    float4 LandscapeTexture1to4IsSnow : packoffset(c9);
    float4 LandscapeTexture5to6IsSnow : packoffset(c10); // bEnableSnowMask in z, inverse iLandscapeMultiNormalTilingFactor in w
    float4 LandscapeTexture1to4IsSpecPower : packoffset(c11);
    float4 LandscapeTexture5to6IsSpecPower : packoffset(c12);
    float4 SnowRimLightParameters : packoffset(c13); // fSnowRimLightIntensity in x, fSnowGeometrySpecPower in y, fSnowNormalSpecPower in z, bEnableSnowRimLighting in w
#endif

#if defined(TRUE_PBR) && defined(LANDSCAPE)
	float3 LandscapeTexture2PBRParams : packoffset(c9);
	float3 LandscapeTexture3PBRParams : packoffset(c10);
	float3 LandscapeTexture4PBRParams : packoffset(c11);
	float3 LandscapeTexture5PBRParams : packoffset(c12);
	float3 LandscapeTexture6PBRParams : packoffset(c13);
#endif

    float4 CharacterLightParams : packoffset(c14);

    uint PBRFlags : packoffset(c15.x);
    float3 PBRParams1 : packoffset(c15.y); // roughness scale, displacement scale, specular level
    float4 PBRParams2 : packoffset(c16); // subsurface color, subsurface opacity

    float3 MaterialObjectRGBScale : packoffset(c17); // RGB multipliers for material objects
};

cbuffer PerGeometry : register(b2)
{
	float4 EyePosition : packoffset(c6);
};

Texture2D<float4> ParallaxHeightmap : register(t0);
SamplerState ParallaxSampler : register(s0);

[domain("tri")]
LightingInOut main(PatchConstantOutput a_patchConstant,
	float3 a_barycentric : SV_DomainLocation,
	const OutputPatch<LightingInOut, INPUT_PATCH_SIZE> a_patch)
{
    LightingInOut output;

	output.Position = Interpolate4(a_barycentric, a_patch[0].Position, a_patch[1].Position, a_patch[2].Position);
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	output.TexCoord0 = Interpolate4(a_barycentric, a_patch[0].TexCoord0, a_patch[1].TexCoord0, a_patch[2].TexCoord0);
#else
	output.TexCoord0 = Interpolate2(a_barycentric, a_patch[0].TexCoord0, a_patch[1].TexCoord0, a_patch[2].TexCoord0);
#endif

#if defined(WORLD_MAP)
	output.InputPosition = Interpolate3(a_barycentric, a_patch[0].InputPosition, a_patch[1].InputPosition, a_patch[2].InputPosition);
#endif

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	output.TBN0 = Interpolate3(a_barycentric, a_patch[0].TBN0, a_patch[1].TBN0, a_patch[2].TBN0);
	output.TBN1 = Interpolate3(a_barycentric, a_patch[0].TBN1, a_patch[1].TBN1, a_patch[2].TBN1);
	output.TBN2 = Interpolate3(a_barycentric, a_patch[0].TBN2, a_patch[1].TBN2, a_patch[2].TBN2);
#endif
#if defined(EYE)
	output.EyeNormal = Interpolate3(a_barycentric, a_patch[0].EyeNormal, a_patch[1].EyeNormal, a_patch[2].EyeNormal);
#elif defined(LANDSCAPE)
	output.LandBlendWeights1 = Interpolate4(a_barycentric, a_patch[0].LandBlendWeights1, a_patch[1].LandBlendWeights1, a_patch[2].LandBlendWeights1);
	output.LandBlendWeights2 = Interpolate4(a_barycentric, a_patch[0].LandBlendWeights2, a_patch[1].LandBlendWeights2, a_patch[2].LandBlendWeights2);
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	output.TexProj = Interpolate3(a_barycentric, a_patch[0].TexProj, a_patch[1].TexProj, a_patch[2].TexProj);
#endif

	output.WorldPosition = Interpolate4(a_barycentric, a_patch[0].WorldPosition, a_patch[1].WorldPosition, a_patch[2].WorldPosition);
	output.PreviousWorldPosition = Interpolate4(a_barycentric, a_patch[0].PreviousWorldPosition, a_patch[1].PreviousWorldPosition, a_patch[2].PreviousWorldPosition);
	output.Color = Interpolate4(a_barycentric, a_patch[0].Color, a_patch[1].Color, a_patch[2].Color);
	output.FogParam = Interpolate4(a_barycentric, a_patch[0].FogParam, a_patch[1].FogParam, a_patch[2].FogParam);
	output.ModelPosition = Interpolate3(a_barycentric, a_patch[0].ModelPosition, a_patch[1].ModelPosition, a_patch[2].ModelPosition);

	float3 e1 = normalize(a_patch[1].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
	float3 e2 = normalize(a_patch[2].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
	float3 faceNormal = normalize(cross(e1, e2));
	
    float height = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord0.xy, 0).x;

#if defined(TRUE_PBR)
	float materialScale = PBRParams1.y;
#else	
    float materialScale = ParallaxOccData.x;
#endif
	
    float3 displacedWorld = output.WorldPosition.xyz + faceNormal * (height - Offset) * Scale * materialScale;
	
	output.WorldPosition.xyz = displacedWorld;
	
    output.Position = mul(FrameBuffer::CameraViewProj, float4(displacedWorld, 1.0));

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	// World-space UV derivatives (meters per UV unit) from the patch corners.
	float2 du1 = a_patch[1].TexCoord0.xy - a_patch[0].TexCoord0.xy;
	float2 du2 = a_patch[2].TexCoord0.xy - a_patch[0].TexCoord0.xy;
	float det = du1.x * du2.y - du2.x * du1.y;

	if (PerturbationScale > 0.0f && abs(det) > 1e-8)
	{
		float2 texOffset = float2(0.002f, 0.002f);
		float heightU = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord0.xy + float2(texOffset.x, 0), 0).x;
		float heightV = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord0.xy + float2(0, texOffset.y), 0).x;

		float3 dp1 = a_patch[1].WorldPosition.xyz - a_patch[0].WorldPosition.xyz;
		float3 dp2 = a_patch[2].WorldPosition.xyz - a_patch[0].WorldPosition.xyz;
		float invDet = 1.0f / det;
		float3 Pu = (dp1 * du2.y - dp2 * du1.y) * invDet;
		float3 Pv = (dp2 * du1.x - dp1 * du2.x) * invDet;

		float slopeU = (heightU - height) * Scale * materialScale * PerturbationScale / (texOffset.x * length(Pu));
		float slopeV = (heightV - height) * Scale * materialScale * PerturbationScale / (texOffset.y * length(Pv));

		float3 T = normalize(output.TBN0);
		float3 B = normalize(output.TBN1);
		float3 N = normalize(output.TBN2);

		float3 perturbedNormal = normalize(N - T * slopeU - B * slopeV);
		T = normalize(T - dot(T, perturbedNormal) * perturbedNormal);
		B = normalize(B - dot(B, perturbedNormal) * perturbedNormal);

		output.TBN0 = T;
		output.TBN1 = B;
		output.TBN2 = perturbedNormal;
	}
#endif

	return output;
}
