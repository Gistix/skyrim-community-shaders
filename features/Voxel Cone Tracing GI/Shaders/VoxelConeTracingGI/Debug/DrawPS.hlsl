#include "VoxelConeTracingGI/Debug/Draw.hlsli"

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    nointerpolation uint3 Coord : TEXCOORD0;    
    nointerpolation float3 Albedo   : COLOR0;
    nointerpolation float3 Normal   : NORMAL; 
    nointerpolation float3 Emissive : COLOR1;    
};

Texture3D<half4> LOD0 : register(t0);

half4 main(PS_INPUT input) : SV_TARGET
{
    half3 output = 0;
    
#if DRAW_MODE == DRAW_ALBEDO
    output = input.Albedo;
#elif DRAW_MODE == DRAW_EMISSIVE
    output = input.Emissive;
#elif DRAW_MODE == DRAW_NORMAL
    output = normalize(input.Normal) * 0.5 + 0.5;
#elif DRAW_MODE == DRAW_LIGHTING || DRAW_MODE == DRAW_FINAL
    output = LOD0[input.Coord].rgb;   
#endif
    
    return half4(output, 1.0f);
}
