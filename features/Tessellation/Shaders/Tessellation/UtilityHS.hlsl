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

struct UtilityInput
{
	float4 PositionCS : SV_POSITION0;

#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 TexCoord0 : TEXCOORD0;
#	endif

#	if defined(RENDER_NORMAL)
	float4 Normal : TEXCOORD1;
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	float3 TexCoord1 : TEXCOORD2;
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 TexCoord1 : TEXCOORD2;
#	elif defined(ADDITIONAL_ALPHA_MASK)
	float2 TexCoord1 : TEXCOORD2;
#	endif

#	if defined(LOCALMAP_FOGOFWAR)
	float Alpha : TEXCOORD3;
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 PositionMS : TEXCOORD5;
#	endif

#	if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	float2 Alpha : TEXCOORD4;
#	elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	float Alpha : TEXCOORD4;
#	endif

#	if defined(DEBUG_SHADOWSPLIT)
	float Depth : TEXCOORD2;
#	endif
#endif
};

PatchConstantOutput PatchConstant(InputPatch<UtilityInput, INPUT_PATCH_SIZE> a_patch)
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
UtilityInput main(InputPatch<UtilityInput, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
