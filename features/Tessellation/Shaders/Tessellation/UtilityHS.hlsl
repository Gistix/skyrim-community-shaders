/*
 * Tessellation feature hull shader for the Utility shader (depth prepass,
 * shadow maps, etc.).
 *
 * Compiled by Tessellation::GetStage() with hs_5_0, keyed on the pass's
 * modified pixel descriptor. All permutation defines come from
 * SIE::SShaderCache::GetShaderDefines, so the struct below matches the
 * Utility shader's VS_OUTPUT layout (package/Shaders/Utility.hlsl) for the
 * exact permutation the bound vertex shader was compiled with. A signature
 * mismatch against the VS output drops the draw.
 *
 * Pass-through for now: fixed tessellation factor from the feature constant
 * buffer (b0). Heightmap bindings and displacement come later.
 */

#include "Tessellation/Common.hlsli"
#include "Tessellation/UtilityCommon.hlsli"

PatchConstantOutput PatchConstant(InputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch)
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
UtilityInOut main(InputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
