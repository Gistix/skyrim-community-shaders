#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"

struct VS_INPUT
{
	float4 Position : POSITION0;
	
#if defined(TEXCOORD)
	#if defined(STRIP_PARTICLES)
		float3 TexCoord0 : TEXCOORD0;
	#else
		float2 TexCoord0 : TEXCOORD0;
	#endif
#endif

#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float4 Normal : NORMAL0;
#endif
#if defined(BINORMAL_TANGENT)
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

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
	float4 SoftMateralVSParams : packoffset(c1);
	float4 FalloffData : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 MatProj[3] : packoffset(c6);
	float4 EyePosition[1] : packoffset(c12);
	float4 PosAdjust[1] : packoffset(c13);
	float4 TexcoordOffsetMembrane : packoffset(c14);
#	else
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 MatProj[3] : packoffset(c12);
	float4 EyePosition[2] : packoffset(c21);
	float4 PosAdjust[2] : packoffset(c23);
	float4 TexcoordOffsetMembrane : packoffset(c25);
#	endif  // VR
}

cbuffer VS_PerFrame : register(b12)
{
#	if !defined(VR)
	row_major float4x4 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#		if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
#			if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[1] : packoffset(c41);
#			endif  // MOTIONVECTORS_NORMALS
#		endif      // SKINNED
#	else
	row_major float4x4 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#		if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
#			if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[2] : packoffset(c82);
#			endif  // MOTIONVECTORS_NORMALS
#		endif      // SKINNED
#	endif          // VR
};

cbuffer IndexedTexcoordBuffer : register(b11)
{
	float4 IndexedTexCoord[128] : packoffset(c0);
}


cbuffer VS_PerFramePS : register(b13)
{
#if !defined(VR)
	float4 CameraPosAdjust[1] : packoffset(c0);
#else
	float4 CameraPosAdjust[2] : packoffset(c0);
#endif  // !VR
}

#	if defined(PROJECTED_UV)
float GetProjectedU(float3 worldPosition, float4 texCoordOffset)
{
	float projUvTmp = min(abs(worldPosition.x), abs(worldPosition.y)) *
	                  (1 / max(abs(worldPosition.x), abs(worldPosition.y)));
	float projUvTmpSqr = projUvTmp * projUvTmp;
	float projUvTmp2 =
		projUvTmpSqr *
			(projUvTmpSqr *
					(projUvTmpSqr * (projUvTmpSqr * 0.0208350997 + -0.0851330012) + 0.180141002) +
				-0.330299497) +
		0.999866009;
	float projUvTmp5;
	if (abs(worldPosition.x) > abs(worldPosition.y)) {
		projUvTmp5 = projUvTmp * projUvTmp2 * -2 + Math::HALF_PI;
	} else {
		projUvTmp5 = 0;
	}
	float projUvTmp6 = projUvTmp * projUvTmp2 + projUvTmp5;
	float projUvTmp7;
	if (worldPosition.y < -worldPosition.y) {
		projUvTmp7 = -Math::PI;
	} else {
		projUvTmp7 = 0;
	}
	float projUvTmp4 = projUvTmp6 + projUvTmp7;
	float minCoord = min(worldPosition.x, worldPosition.y);
	float maxCoord = max(worldPosition.x, worldPosition.y);
	if (minCoord < -minCoord && maxCoord >= -maxCoord) {
		projUvTmp4 = -projUvTmp4;
	}
	return abs(0.318309158 * projUvTmp4) * texCoordOffset.w + texCoordOffset.y;
}

float GetProjectedV(float3 worldPosition, uint a_eyeIndex = 0)
{
	return (-PosAdjust[a_eyeIndex].x + (PosAdjust[a_eyeIndex].z + worldPosition.z)) / PosAdjust[a_eyeIndex].y;
}
#	endif

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
	
#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float3 normal = input.Normal.xyz * 2 - 1;

	#if defined(SKINNED)
		float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
		float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
	
		float3 worldNormal = normalize(mul(normal, boneRSMatrixTr));
	#else
		float3 worldNormal = normalize(mul((float3x3)World[eyeIndex], normal));
	#endif
#else
	float3 worldNormal = float3(0, 0, 1);
#endif	

	vsout.NormalWS = worldNormal;

	float4 texCoord = float4(0, 0, 1, 0);

#if defined(MEMBRANE)
	float4 texCoordOffset = TexcoordOffsetMembrane;
#else // !MEMBRANE
	float4 texCoordOffset = TexcoordOffset;
#endif // MEMBRANE
	
#if defined(TEXCOORD) && defined(TEXCOORD_INDEX)
	#if defined(STRIP_PARTICLES) && defined(NORMALS)
		uint index = input.TexCoord0.z;
	#else // !NORMALS
		uint index = input.Position.w;
	#endif // NORMALS
#endif // TEXCOORD_INDEX
	
#if defined(PROJECTED_UV)
	#if defined(NORMALS) && !defined(MEMBRANE)
		texCoord.x = dot(MatProj[0].xyz, inputPosition.xyz);
	#else
		texCoord.x = GetProjectedU(worldPosition.xyz, texCoordOffset);
	#endif
#else // !PROJECTED_UV
	#if defined(TEXCOORD) && defined(TEXTURE)
		float u = input.TexCoord0.x;
		#if defined(TEXCOORD_INDEX)
			u = IndexedTexCoord[index].y * u + IndexedTexCoord[index].x;
		#endif // TEXCOORD_INDEX
		texCoord.x = u * texCoordOffset.z + texCoordOffset.x;
	#endif // TEXTURE
#endif // PROJECTED_UV

#if defined(PROJECTED_UV)
	#if defined(NORMALS) && !defined(MEMBRANE)
		texCoord.y = dot(MatProj[1].xyz, inputPosition.xyz);
	#else
		texCoord.y = GetProjectedV(worldPosition.xyz, eyeIndex);
	#endif
#else // !PROJECTED_UV
	#if defined(TEXCOORD) && defined(TEXTURE)
		float v = input.TexCoord0.y;
		#if defined(TEXCOORD_INDEX)
			v = IndexedTexCoord[index].w * v + IndexedTexCoord[index].z;
		#endif // TEXCOORD_INDEX
		texCoord.y = v * texCoordOffset.w + texCoordOffset.y;
	#endif // TEXTURE
#endif // PROJECTED_UV

#if defined(TEXCOORD) && defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.w = input.TexCoord0.y;
#elif defined(SOFT)
	texCoord.w = viewPos.w / SoftMateralVSParams.x;
#elif defined(TEXCOORD) && defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.w = input.TexCoord0.y;
#endif

#if defined(TEXCOORD) && defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.z = input.TexCoord0.x;
#elif defined(FALLOFF)
	float3 inverseWorldDirection = normalize(-worldPosition.xyz);
	float WdotN = dot(worldNormal, inverseWorldDirection);
	float falloff = saturate((-FalloffData.x + abs(WdotN)) / (FalloffData.y - FalloffData.x));
	float falloffParam = (falloff * falloff) * (3 - falloff * 2);
	texCoord.z = lerp(FalloffData.z, FalloffData.w, falloffParam);
#elif defined(TEXCOORD) && defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.z = input.TexCoord0.x;
#endif

	vsout.TexCoord0 = texCoord;	

#if defined(VC)
	vsout.Color = input.Color;
#else
	vsout.Color = 1.0.xxxx;
#endif  // VC	
	
	return vsout;
}