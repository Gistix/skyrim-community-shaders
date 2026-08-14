#ifndef LIGHTING_COMMON_HLSLI
#define LIGHTING_COMMON_HLSLI

struct LightingInOut
{
    float4 Position : SV_POSITION0;
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	float4 TexCoord0 : TEXCOORD0;
#else
    float2 TexCoord0 : TEXCOORD0;
#endif

#if defined(WORLD_MAP)
	float3 InputPosition : TEXCOORD4;
#endif

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
    float3 TBN0 : TEXCOORD1;
    float3 TBN1 : TEXCOORD2;
    float3 TBN2 : TEXCOORD3;
#endif
#if defined(EYE)
	float3 EyeNormal : TEXCOORD6;
#elif defined(LANDSCAPE)
	float4 LandBlendWeights1 : TEXCOORD6;
	float4 LandBlendWeights2 : TEXCOORD7;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3 TexProj : TEXCOORD7;
#endif

    float4 WorldPosition : POSITION1;
    float4 PreviousWorldPosition : POSITION2;
    float4 Color : COLOR0;
    float4 FogParam : COLOR1;

    float3 ModelPosition : TEXCOORD12;
};

#endif  // LIGHTING_COMMON_HLSLI
