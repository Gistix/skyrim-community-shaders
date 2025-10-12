struct VS_INPUT
{
	float4 Position		: POSITION;
	float2 TexCoord0	: TEXCOORD0;
	float3 Normal		: NORMAL;
	float3 Binormal		: BINORMAL;
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
#	if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 EyePosition[1] : packoffset(c6);
	float4 LandBlendParams : packoffset(c7);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c8);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c9);
	row_major float3x4 TextureProj[1] : packoffset(c10);
	float IndexScale : packoffset(c13);
	float4 WorldMapOverlayParameters : packoffset(c14);
#	else   // VR has 49 vs 30 entries
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 EyePosition[2] : packoffset(c12);
	float4 LandBlendParams : packoffset(c14);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c15);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c16);
	row_major float3x4 TextureProj[2] : packoffset(c17);
	float IndexScale : packoffset(c23);
	float4 WorldMapOverlayParameters : packoffset(c24);
#	endif  // VR
};

VS_OUTPUT main(VS_INPUT input)
{
	precise float4 inputPosition = float4(input.Position.xyz, 1.0);
	
	VS_OUTPUT vsout;
	vsout.PositionWS = float4(mul(World[0], inputPosition), 1.0);
	//vsout.TexCoord0 = input.TexCoord0;

	float4 uv = float4(input.TexCoord0 * TexcoordOffset.zw + TexcoordOffset.xy, 0, 0);
#if defined(LANDSCAPE)
	vsout.TexCoord0.zw = (uv * 0.010416667.xx + LandBlendParams.xy) * float2(1, -1) + float2(0, 1);
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexCoord0.z = mul(TextureProj[eyeIndex][0], inputPosition);
	vsout.TexCoord0.w = mul(TextureProj[eyeIndex][1], inputPosition);
#endif
	vsout.TexCoord0 = uv;
	
	vsout.NormalWS = normalize(mul((float3x3)World[0], input.Normal.xyz));
	vsout.Color = float4(0, 0, 0, 0);
	return vsout;
}