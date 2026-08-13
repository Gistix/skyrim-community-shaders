/*
 * Tessellation feature hull shader for the Lighting shader.
 *
 * Compiled by Tessellation::GetStage() with hs_5_0, keyed on the pass's
 * modified pixel descriptor. The HULLSHADER define is injected by
 * Util::CompileShader; all other permutation defines (SKINNED,
 * MODELSPACENORMALS, WORLD_MAP, EYE, LANDSCAPE, PROJECTED_UV, ...) come
 * from SIE::SShaderCache::GetShaderDefines, so the struct below matches the
 * Lighting shader's VS_OUTPUT layout (package/Shaders/Lighting.hlsl) for the
 * exact permutation the bound vertex shader was compiled with. A signature
 * mismatch against the VS output drops the draw.
 */

#include "Tessellation/Common.hlsli"

struct HullInput
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

PatchConstantOutput PatchConstant(InputPatch<HullInput, INPUT_PATCH_SIZE> a_patch)
{
	PatchConstantOutput output;
	float factor = max(TessellationFactor, 1.0);
	output.tessFactor[0] = factor;
	output.tessFactor[1] = factor;
	output.tessFactor[2] = factor;
	output.insideTessFactor = factor;
	return output;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(INPUT_PATCH_SIZE)]
[patchconstantfunc("PatchConstant")]
HullInput main(InputPatch<HullInput, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
