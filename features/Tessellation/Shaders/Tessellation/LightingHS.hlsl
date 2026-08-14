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
#include "Tessellation/LightingCommon.hlsli"

PatchConstantOutput PatchConstant(InputPatch<LightingInOut, INPUT_PATCH_SIZE> a_patch)
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
LightingInOut main(InputPatch<LightingInOut, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
