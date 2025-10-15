#include "Common/FrameBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Skinned.hlsli"

struct VS_INPUT
{
	float4 Position		: POSITION;
	
#if defined(TEXTURE)
	float2 TexCoord : TEXCOORD0;
#endif

#if defined(NORMALS)
	float4 Normal : NORMAL0;
	float4 Bitangent : BINORMAL0;
#endif
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 PositionWS	: POSITION;
	float4 TexCoord0	: TEXCOORD0;
	float3 NormalWS		: NORMAL;
	float4 Color		: COLOR0;
};

cbuffer PerTechnique : register(b0)
{
#	if !defined(VR)
	float4 HighDetailRange[1] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c1);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#	else
	float4 HighDetailRange[2] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c2);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#	endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c1);
	float4 EyePos[1] : packoffset(c5);
	float4 WaterParams : packoffset(c6);
	float4 TreeParams : packoffset(c7);
#	else
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c1);
	float4 EyePos[2] : packoffset(c9);
	float4 WaterParams : packoffset(c11);
	float4 TreeParams : packoffset(c12);
#	endif  // VR
};

float2 SmoothSaturate(float2 value)
{
	return value * value * (3 - 2 * value);
}

VS_OUTPUT main(VS_INPUT input)
{
	uint eyeIndex = 0;
	precise float4 positionMS = float4(input.Position.xyz, 1.0);
	
	float3 normalMS = float3(1, 1, 1);
	#if defined(NORMALS)
		normalMS = input.Normal.xyz * 2 - 1;
	#endif

	#if defined(VC) && defined(NORMALS) && defined(TREE_ANIM)
		float2 treeTmp1 = SmoothSaturate(abs(2 * frac(float2(0.1, 0.25) * (TreeParams.w * TreeParams.y * TreeParams.x) + dot(input.Position.xyz, 1.0.xxx) + 0.5) - 1));
		float normalMult = (treeTmp1.x + 0.1 * treeTmp1.y) * (input.Color.w * TreeParams.z);
		positionMS.xyz += normalMS.xyz * normalMult;
	#endif

	#if defined(LOD_LANDSCAPE)
		positionMS = LodLandscape::AdjustLodLandscapeVertexPositionMS(positionMS, World[eyeIndex], HighDetailRange[eyeIndex]);
	#endif

	#if defined(SKINNED)
		precise int4 boneIndices = 765.01.xxxx * input.BoneIndices.xyzw;

		float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, boneIndices, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, input.BoneWeights);
		precise float4 positionWS = float4(mul(positionMS, transpose(worldMatrix)), 1);
		float3x3 normalMatrix = (float3x3)worldMatrix; 
	#else
		precise float4 positionWS = mul(World[eyeIndex], positionMS);
		float3x3 normalMatrix = (float3x3)World[0];  
	#endif
	positionWS.xyz += FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 normalWS = normalize(mul(normalMatrix, normalMS));
	
	VS_OUTPUT vsout;
	vsout.PositionWS = positionWS;
	vsout.NormalWS = normalWS;
	
	#if defined(TEXTURE)
		vsout.TexCoord0 = float4(input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy, 0, 0);
	#else
		vsout.TexCoord0 = 0;
	#endif

#if defined(VC)
	vsout.Color = input.Color;
#else
	vsout.Color = 1.0.xxxx;
#endif  // VC		
	
	return vsout;
}