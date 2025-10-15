struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float4 Albedo   : COLOR0;
    float4 Normal   : NORMAL;  
    float4 Emissive : COLOR1;    
};

struct PS_OUT
{
    float4 Albedo   : SV_Target0;   
    float4 Normal   : SV_Target1;
    float4 Emissive : SV_Target2;     
};

PS_OUT main(PS_INPUT input)
{
    PS_OUT output;
    
    output.Albedo = input.Albedo;
    output.Normal = input.Normal;
    output.Emissive = input.Emissive;
    
    return output;
}
