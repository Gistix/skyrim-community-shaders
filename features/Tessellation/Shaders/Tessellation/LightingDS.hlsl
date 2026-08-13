/*
 * Tessellation feature domain shader for the Lighting shader.
 *
 * Compiled by Tessellation::GetStage() with ds_5_0, keyed on the pass's
 * modified pixel descriptor. The DOMAINSHADER define is injected by
 * Util::CompileShader; all other permutation defines (SKINNED,
 * MODELSPACENORMALS, WORLD_MAP, EYE, LANDSCAPE, PROJECTED_UV, ...) come
 * from SIE::SShaderCache::GetShaderDefines, so the output struct matches the
 * Lighting shader's VS_OUTPUT layout (package/Shaders/Lighting.hlsl) and the
 * game pixel shader consumes it directly.
 */

#include "Tessellation/Common.hlsli"

// Mirrors of the game's bound buffers (see Tessellation::MirrorParallaxBindings).
// Partial layouts: only the members this shader reads are declared, with
// matching packoffsets so the runtime maps them correctly.
cbuffer PerMaterial : register(b1)
{
	float4 ParallaxOccData : packoffset(c3);
};

cbuffer PerGeometry : register(b2)
{
	float4 EyePosition : packoffset(c6);
};

cbuffer VS_PerFrame : register(b12)
{
	row_major float4x4 ViewProj : packoffset(c8);
};

#if defined(PARALLAX) || defined(PARALLAX_OCC)
// The game binds the parallax heightmap at PS slot 3 for vanilla parallax
// materials (BSLightingShader::SetupMaterial, MatSetTextureSlot(3, ...)).
Texture2D<float4> ParallaxHeightmap : register(t3);
SamplerState ParallaxSampler : register(s3);
#elif defined(TRUE_PBR)
// PBR materials bind their displacement texture at PS slot 4
// (TruePBR.cpp, SetPSTexture(4, ...)).
Texture2D<float4> ParallaxHeightmap : register(t4);
SamplerState ParallaxSampler : register(s4);
#endif

struct DomainInput
{
	float4 Position : SV_POSITION0;
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	float4 TexCoord0 : TEXCOORD0;
#else
	float2 TexCoord0 : TEXCOORD0;
#endif

#if defined(WORLD_MAP)
	float3 InputPosition : TEXCOORD4;
#endif

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3 TBN0 : TEXCOORD1;
	float3 TBN1 : TEXCOORD2;
	float3 TBN2 : TEXCOORD3;
#endif
#if defined(EYE)
	float3 EyeNormal : TEXCOORD6;
#elif defined(LANDSCAPE)
	float4 LandBlendWeights1 : TEXCOORD6;
	float4 LandBlendWeights2 : TEXCOORD7;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3 TexProj : TEXCOORD7;
#endif

	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
	float4 Color : COLOR0;
	float4 FogParam : COLOR1;

	float3 ModelPosition : TEXCOORD12;
};

[domain("tri")]
DomainInput main(PatchConstantOutput a_patchConstant,
	float3 a_barycentric : SV_DomainLocation,
	const OutputPatch<DomainInput, INPUT_PATCH_SIZE> a_patch)
{
	DomainInput output;

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

#if defined(PARALLAX) || defined(PARALLAX_OCC) || defined(TRUE_PBR)
	// True displacement: displace the interpolated world position along the
	// patch's geometric normal (from the raw control point winding, edges
	// normalized first for stability at camera-relative coordinates) by the
	// parallax height (sampled like the PS does, .x channel), then recompute
	// the clip-space position.
	float height = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord0.xy, 0).x;
	float3 e1 = normalize(a_patch[1].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
	float3 e2 = normalize(a_patch[2].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
	float3 faceNormal = normalize(cross(e1, e2));
	float3 displacedWorld = output.WorldPosition.xyz + faceNormal * (height - 0.5) * TessellationScale;
	output.WorldPosition.xyz = displacedWorld;
	// Matrix-first mul to match the game's VS convention (mul(ViewProj, ...)
	// in Lighting.hlsl); mul(v, M) would apply the transposed transform and
	// collapse the geometry toward screen center.
	output.Position = mul(ViewProj, float4(displacedWorld, 1.0));
#endif

	return output;
}
