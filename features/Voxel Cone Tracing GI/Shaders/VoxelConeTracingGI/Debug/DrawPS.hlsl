#include "VoxelConeTracingGI/Debug/Draw.hlsli"

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float3 Albedo   : COLOR0;
    float3 Emission : COLOR1;
    float3 Normal   : NORMAL;    
};

half4 main(PS_INPUT input) : SV_TARGET
{
    half3 output;
    
#if DRAW_MODE == DRAW_ALBEDO
    output = input.Albedo;
#elif DRAW_MODE == DRAW_EMISSION
    output = input.Emission;
#elif DRAW_MODE == DRAW_NORMAL
    output = normalize(input.Normal) * 0.5 + 0.5;
#endif
    
    return half4(output, 1.0f);
}
