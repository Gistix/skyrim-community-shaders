struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float4 Albedo   : COLOR0;
    float4 Normal   : NORMAL;  
    float4 Emission : COLOR1;    
};

struct PS_OUT
{
    float4 Albedo   : SV_Target0;   
    float4 Normal   : SV_Target1;
    float4 Emission : SV_Target2;     
};

PS_OUT main(PS_INPUT input)
{
    PS_OUT output;
    
    output.Albedo = input.Albedo;
    output.Emission = input.Emission;
    output.Normal = input.Normal;
    
    return output;
}
