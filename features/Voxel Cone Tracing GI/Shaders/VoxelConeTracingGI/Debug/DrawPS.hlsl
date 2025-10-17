#include "VoxelConeTracingGI/Debug/Draw.hlsli"

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    uint3 Coord : TEXCOORD0;    
    float3 Albedo   : COLOR0;
    float3 Normal   : NORMAL; 
    float3 Emission : COLOR1;    
};

Texture3D<half4> LOD0 : register(t0);

half4 main(PS_INPUT input) : SV_TARGET
{
    half3 output = 0;
    
#if DRAW_MODE == DRAW_ALBEDO
    output = input.Albedo;
#elif DRAW_MODE == DRAW_EMISSION
    output = input.Emission;
#elif DRAW_MODE == DRAW_NORMAL
    output = normalize(input.Normal) * 0.5 + 0.5;
#elif DRAW_MODE == DRAW_LIGHTING || DRAW_MODE == DRAW_FINAL
    output = LOD0[input.Coord].rgb;   
#endif
    
    return half4(output, all(output < 0.001) ? 0.0f : 1.0f);
}
