#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

struct GS_INPUT
{
	float4 PositionWS   : POSITION;
	float4 TexCoord0    : TEXCOORD0;
	float3 NormalWS     : NORMAL;
	half4 Color         : COLOR0;
    uint ClipmapIdx     : INSTANCE_ID;
};

struct GS_OUTPUT
{
    float4 Position                 : SV_POSITION;
	centroid float4 TexCoord0       : TEXCOORD0;
	centroid float3 NormalWS        : NORMAL;
	centroid float4 Color           : COLOR0;
    centroid float3 Cell            : TEXCOORD1;
    centroid float3 PositionWS      : TEXCOORD2;
    nointerpolation uint ClipmapIdx : INSTANCE_ID;
    
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	nointerpolation float3 AABBMin : AABBMIN;
	nointerpolation float3 AABBMax : AABBMAX;
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED    
};

cbuffer VCTGICB : register(b0)
{
    VoxelConeTracingGI::ConstantBuffer VCTGI;
};

cbuffer ClipmapCB : register(b1)
{
    VoxelConeTracingGI::ClipmapConstantBuffer ClipmapData;
};

[maxvertexcount(3)]
void main(triangle GS_INPUT input[3], inout TriangleStream<GS_OUTPUT> outputStream)
{
    float3 facenormal = abs(input[0].NormalWS + input[1].NormalWS + input[2].NormalWS);

    uint maxi = facenormal.y > facenormal.x ? 1 : 0;
    maxi = facenormal.z > facenormal[maxi] ? 2 : maxi;
    
 	float3 aabbMin = min(input[0].PositionWS.xyz, min(input[1].PositionWS.xyz, input[2].PositionWS.xyz));
	float3 aabbMax = max(input[0].PositionWS.xyz, max(input[1].PositionWS.xyz, input[2].PositionWS.xyz));
    
    uint clipIdx = ClipmapData.Start + input[0].ClipmapIdx; // Special thanks to Microsoft Corporation
    VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[clipIdx];

    // Early out if triangle is outside volume
    [branch]
    if (!IntersectAABB(clipmap.Min, clipmap.Max, aabbMin, aabbMax))
        return;
      
    GS_OUTPUT output[3];
    
    uint i = 0;
    [unroll]
    for (i = 0; i < 3; ++i)
    {
        float3 uvw = ((input[i].PositionWS.xyz - clipmap.Min) * clipmap.SizeRcp) * RCP_SCALE; // [0..1]
        float3 coords = uvw * VCTGI.Res; // [0..Res]
        
        output[i].Cell = coords; 
        
        // World space -> Voxel grid space:
        output[i].Position.xyz = (coords * 2.0) - VCTGI.Res.xxx; // [-Res..Res]

		// Project onto dominant axis:
		[flatten]
        if (maxi == 0)
        {
            output[i].Position.xyz = output[i].Position.zyx;
        }
        else if (maxi == 1)
        {
            output[i].Position.xyz = output[i].Position.xzy;
        }
    }
    
    #ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	    // Expand triangle to get fake Conservative Rasterization:
	    float2 side0N = normalize(output[1].Position.xy - output[0].Position.xy);
	    float2 side1N = normalize(output[2].Position.xy - output[1].Position.xy);
	    float2 side2N = normalize(output[0].Position.xy - output[2].Position.xy);
	    output[0].Position.xy += normalize(side2N - side0N);
	    output[1].Position.xy += normalize(side0N - side1N);
	    output[2].Position.xy += normalize(side1N - side2N);
    #endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
    
    [unroll]
    for (i = 0; i < 3; ++i)  
    {
        // Voxel grid space -> Clip space
        output[i].Position.xy = (output[i].Position.xy * VCTGI.ResRcp) * RCP_SCALE;
        output[i].Position.z = 1.0f;
		output[i].Position.w = 1.0f;
        
        output[i].TexCoord0 = input[i].TexCoord0;
        
        output[i].NormalWS = input[i].NormalWS;
        output[i].Color = input[i].Color;
        output[i].PositionWS = input[i].PositionWS.xyz;
        output[i].ClipmapIdx = clipIdx;
         
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	    output[i].AABBMin = aabbMin;
	    output[i].AABBMax = aabbMax;
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED      
         
        outputStream.Append(output[i]);    
    }
}