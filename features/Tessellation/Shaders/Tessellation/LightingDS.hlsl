#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/LightingCommon.hlsli"

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
    float3 displacedWorld = output.WorldPosition.xyz + faceNormal * (height - Offset) * Scale;
	
	output.WorldPosition.xyz = displacedWorld;
	
    output.Position = mul(FrameBuffer::CameraViewProj, float4(displacedWorld, 1.0));

	return output;
}
