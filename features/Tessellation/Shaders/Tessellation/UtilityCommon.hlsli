#ifndef UTILITY_COMMON_HLSLI
#define UTILITY_COMMON_HLSLI

struct UtilityInOut
{
    float4 PositionCS : SV_POSITION0;
	
#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 TexCoord0: TEXCOORD0;
#endif
	
#if defined(RENDER_NORMAL)
	float4 Normal : TEXCOORD1;
#endif

#if defined(RENDER_SHADOWMAP_PB)
	float3 TexCoord1 : TEXCOORD2;
#elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 TexCoord1 : TEXCOORD2;
#elif defined(ADDITIONAL_ALPHA_MASK)
	float2 TexCoord1 : TEXCOORD2;
#endif

#if defined(LOCALMAP_FOGOFWAR)
	float Alpha : TEXCOORD3;
#endif

#if defined(RENDER_SHADOWMASK_ANY)
	float4 PositionMS : TEXCOORD5;
#endif

#if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	float2 Alpha : TEXCOORD4;
#elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	float Alpha : TEXCOORD4;
#endif

#if defined(DEBUG_SHADOWSPLIT)
	float Depth : TEXCOORD2;
#endif
#endif

    float2 TexCoord2 : TEXCOORD6;
    float4 WorldPosition : POSITION1;
};

#endif  // UTILITY_COMMON_HLSLI
