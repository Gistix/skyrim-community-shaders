#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"

struct VS_INPUT
{
	float4 Position		: POSITION;
	float2 TexCoord0	: TEXCOORD0;
#if !defined(MODELSPACENORMALS)
	float4 Normal : NORMAL0;
	float4 Bitangent : BINORMAL0;
#endif  // !MODELSPACENORMALS

#if defined(VC)
	float4 Color : COLOR0;
#	if defined(LANDSCAPE)
	float4 LandBlendWeights1 : TEXCOORD2;
	float4 LandBlendWeights2 : TEXCOORD3;
#	endif  // LANDSCAPE
#endif      // VC
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif  // SKINNED
};

struct VS_OUTPUT
{
	float4 PositionWS	: POSITION;
	float4 TexCoord0	: TEXCOORD0;
	float3 NormalWS		: NORMAL;
	float4 Color		: COLOR0;
};

cbuffer PerMaterial : register(b1)
{
	float4 LeftEyeCenter : packoffset(c0);
	float4 RightEyeCenter : packoffset(c1);
	float4 TexcoordOffset : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 EyePosition[1] : packoffset(c6);
	float4 LandBlendParams : packoffset(c7);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c8);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c9);
	row_major float3x4 TextureProj[1] : packoffset(c10);
	float IndexScale : packoffset(c13);
	float4 WorldMapOverlayParameters : packoffset(c14);
#else   // VR has 49 vs 30 entries
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 EyePosition[2] : packoffset(c12);
	float4 LandBlendParams : packoffset(c14);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c15);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c16);
	row_major float3x4 TextureProj[2] : packoffset(c17);
	float IndexScale : packoffset(c23);
	float4 WorldMapOverlayParameters : packoffset(c24);
#endif  // VR
};

cbuffer VS_PerFrame : register(b12)
{
#	if !defined(VR)
	row_major float3x3 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#		if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
	float3 PreviousBonesPivot[1] : packoffset(c41);
#		endif  // SKINNED
#	else
	row_major float3x3 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#		if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
	float3 PreviousBonesPivot[2] : packoffset(c82);
#		endif  // SKINNED
#	endif      // VR
};

cbuffer VS_PerFramePS : register(b13)
{
#if !defined(VR)
	float4 CameraPosAdjust[1] : packoffset(c0);
#else
	float4 CameraPosAdjust[2] : packoffset(c0);
#endif  // !VR
}

VS_OUTPUT main(VS_INPUT input)
{
	uint eyeIndex = 0;
	precise float4 inputPosition = float4(input.Position.xyz, 1.0);
	
#if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;

	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot[eyeIndex], input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(worldMatrix)), 1);
#else   // !SKINNED
	precise float4 worldPosition = float4(mul(World[eyeIndex], inputPosition), 1);
#endif  // SKINNED	

	// Copied from framebuffer, if we use the original here it will clash with VS_PerFrame (same constant buffer slot)
	worldPosition.xyz += CameraPosAdjust[eyeIndex].xyz;

	VS_OUTPUT vsout;
	vsout.PositionWS = worldPosition;

	float4 uv = float4(input.TexCoord0 * TexcoordOffset.zw + TexcoordOffset.xy, 0, 0);
#if defined(LANDSCAPE)
	vsout.TexCoord0.zw = (uv * 0.010416667.xx + LandBlendParams.xy) * float2(1, -1) + float2(0, 1);
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexCoord0.z = mul(TextureProj[eyeIndex][0], inputPosition);
	vsout.TexCoord0.w = mul(TextureProj[eyeIndex][1], inputPosition);
#endif
	vsout.TexCoord0 = uv;
	
#if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
#endif	
	
#if defined(MODELSPACENORMALS)
    vsout.NormalWS = float3(0, 0, 1); // Or any debug normal direction
#else // MODELSPACENORMALS not defined

    #if defined(SKINNED)
        // Skinned mesh: transform normal using boneRSMatrix
        float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
        float3x3 normalMatrix = transpose(
			float3x3(
				normalize(boneRSMatrixTr[0]),
				normalize(boneRSMatrixTr[1]),
				normalize(boneRSMatrixTr[2])
			)
		);
    #else
        float3x3 normalMatrix = (float3x3)World[0];     
    #endif
	
	vsout.NormalWS = normalize(mul(normalMatrix, input.Normal.xyz));
#endif

#if defined(VC)
	vsout.Color = input.Color;
#else
	vsout.Color = 1.0.xxxx;
#endif  // VC	
	
	return vsout;
}