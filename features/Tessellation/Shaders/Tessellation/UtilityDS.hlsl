#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/UtilityCommon.hlsli"

Texture2D<float4> ParallaxHeightmap : register(t0);
SamplerState ParallaxSampler : register(s0);

[domain("tri")]
UtilityInOut main(PatchConstantOutput a_patchConstant,
	float3 a_barycentric : SV_DomainLocation,
	const OutputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch)
{
    UtilityInOut output;

	output.PositionCS = Interpolate4(a_barycentric, a_patch[0].PositionCS, a_patch[1].PositionCS, a_patch[2].PositionCS);


#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
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

    output.TexCoord2 = Interpolate2(a_barycentric, a_patch[0].TexCoord2, a_patch[1].TexCoord2, a_patch[2].TexCoord2);
    output.WorldPosition = Interpolate4(a_barycentric, a_patch[0].WorldPosition, a_patch[1].WorldPosition, a_patch[2].WorldPosition);
	
    float3 e1 = normalize(a_patch[1].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
    float3 e2 = normalize(a_patch[2].WorldPosition.xyz - a_patch[0].WorldPosition.xyz);
    float3 faceNormal = normalize(cross(e1, e2));
	
    float height = ParallaxHeightmap.SampleLevel(ParallaxSampler, output.TexCoord2.xy, 0).x;
    float3 displacedWorld = output.WorldPosition.xyz + faceNormal * (height - Offset) * Scale;
	
    output.WorldPosition.xyz = displacedWorld;
	
	output.PositionCS = mul(FrameBuffer::CameraViewProj, float4(displacedWorld, 1.0));

	return output;
}
