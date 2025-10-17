#include "VoxelConeTracingGI/Debug/Draw.hlsli"

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float3 Albedo   : COLOR0;
    float3 Normal   : NORMAL; 
    float3 Emission : COLOR1;    
};

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
    float3 normal = normalize(input.Normal);
    output = input.Albedo * saturate(dot(float3(0, 0, 1), normal));
    
    #if DRAW_MODE == DRAW_FINAL
        output += input.Emission;
    #endif       
#endif
    
    return half4(output, all(output < 0.001) ? 0.0f : 1.0f);
}
