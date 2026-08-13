/*
 * Tessellation feature domain shader for the Utility shader (depth prepass,
 * shadow maps, etc.).
 *
 * Compiled by Tessellation::GetStage() with ds_5_0, keyed on the pass's
 * modified pixel descriptor. The output struct mirrors the Utility shader's
 * VS_OUTPUT layout (package/Shaders/Utility.hlsl) so downstream stages
 * consume it directly.
 *
 * Pass-through for now; heightmap bindings and displacement come later.
 */

#include "Tessellation/Common.hlsli"
#include "Common/FrameBuffer.hlsli"

// The utility path always binds the heightmap at PS slot 4 (see
// Tessellation::BSUtilityShader_SetupMaterial); the utility descriptor
// carries no technique flags, so this slot is unconditional.
Texture2D<float4> ParallaxHeightmap : register(t4);
SamplerState ParallaxSampler : register(s4);

struct UtilityOutput
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

[domain("tri")]
UtilityOutput main(PatchConstantOutput a_patchConstant,
	float3 a_barycentric : SV_DomainLocation,
	const OutputPatch<UtilityOutput, INPUT_PATCH_SIZE> a_patch)
{
	UtilityOutput output;

	output.PositionCS = Interpolate4(a_barycentric, a_patch[0].PositionCS, a_patch[1].PositionCS, a_patch[2].PositionCS);

#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	output.TexCoord0 = Interpolate4(a_barycentric, a_patch[0].TexCoord0, a_patch[1].TexCoord0, a_patch[2].TexCoord0);
#	endif

#	if defined(RENDER_NORMAL)
	output.Normal = Interpolate4(a_barycentric, a_patch[0].Normal, a_patch[1].Normal, a_patch[2].Normal);
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	output.TexCoord1 = Interpolate3(a_barycentric, a_patch[0].TexCoord1, a_patch[1].TexCoord1, a_patch[2].TexCoord1);
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	output.TexCoord1 = Interpolate4(a_barycentric, a_patch[0].TexCoord1, a_patch[1].TexCoord1, a_patch[2].TexCoord1);
#	elif defined(ADDITIONAL_ALPHA_MASK)
	output.TexCoord1 = Interpolate2(a_barycentric, a_patch[0].TexCoord1, a_patch[1].TexCoord1, a_patch[2].TexCoord1);
#	endif

#	if defined(LOCALMAP_FOGOFWAR)
	output.Alpha = Interpolate1(a_barycentric, a_patch[0].Alpha, a_patch[1].Alpha, a_patch[2].Alpha);
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	output.PositionMS = Interpolate4(a_barycentric, a_patch[0].PositionMS, a_patch[1].PositionMS, a_patch[2].PositionMS);
#	endif

#	if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	output.Alpha = Interpolate2(a_barycentric, a_patch[0].Alpha, a_patch[1].Alpha, a_patch[2].Alpha).x;
#	elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	output.Alpha = Interpolate1(a_barycentric, a_patch[0].Alpha, a_patch[1].Alpha, a_patch[2].Alpha);
#	endif

#	if defined(DEBUG_SHADOWSPLIT)
	output.Depth = Interpolate1(a_barycentric, a_patch[0].Depth, a_patch[1].Depth, a_patch[2].Depth);
#	endif
#endif

#if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	// The Utility VS does not output world positions, so reconstruct the
	// camera-relative control points from clip space and displace along the
	// patch face normal, mirroring the Lighting DS. This requires UVs, which
	// are only present in the permutations guarded above; the base depth
	// prepass permutation has no TexCoord0 and stays pass-through.
	float4 world0 = mul(FrameBuffer::CameraViewProjInverse, a_patch[0].PositionCS);
	float4 world1 = mul(FrameBuffer::CameraViewProjInverse, a_patch[1].PositionCS);
	float4 world2 = mul(FrameBuffer::CameraViewProjInverse, a_patch[2].PositionCS);
	float3 e1 = normalize(world1.xyz - world0.xyz);
	float3 e2 = normalize(world2.xyz - world0.xyz);
	float3 faceNormal = normalize(cross(e1, e2));
	float3 worldPos = Interpolate3(a_barycentric, world0.xyz, world1.xyz, world2.xyz);
	float height = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord0.xy, 0).x;
	float3 displacedWorld = worldPos + faceNormal * (height - 0.5) * TessellationScale;
	output.PositionCS = mul(FrameBuffer::CameraViewProj, float4(displacedWorld, 1.0));
#endif

	return output;
}
