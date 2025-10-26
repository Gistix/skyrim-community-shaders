#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Debug/Draw.hlsli"

struct GS_INPUT
{
    float4 Position : SV_POSITION;
    nointerpolation uint3 Coord     : TEXCOORD0;   
    nointerpolation float3 Albedo   : COLOR0;
    nointerpolation float3 Normal   : NORMAL;
    nointerpolation float3 Emissive : COLOR1;
    nointerpolation float3 VoxelPosition : TEXCOORD1;   
};

struct GS_OUTPUT
{
    float4 Position : SV_POSITION;
    nointerpolation uint3 Coord     : TEXCOORD0;    
    nointerpolation float3 Albedo   : COLOR0;
    nointerpolation float3 Normal   : NORMAL; 
    nointerpolation float3 Emissive : COLOR1;    
};

cbuffer VCTGICB : register(b0)
{
	VoxelConeTracingGI::ConstantBuffer VCTGI;
}


cbuffer ClipmapCB : register(b1)
{
    VoxelConeTracingGI::ClipmapConstantBuffer Clipmap;
};

[maxvertexcount(3)]
void main(triangle GS_INPUT input[3], inout TriangleStream<GS_OUTPUT> outputStream)
{
    uint clipIdx = Clipmap.Index;

    // Early out if triangle is inside previous clipmap
    /*[branch]
    if (clipIdx > 0) {
        VoxelConeTracingGI::Clipmap prevClipmap = VCTGI.Clipmaps[clipIdx - 1];
        
        float3 position = input[0].VoxelPosition;
        
        if (all(position >= prevClipmap.Min) && all(position <= prevClipmap.Max)) {
            return;
        }
    }*/

        
    GS_OUTPUT output[3];
        
    [unroll]
    for (uint i = 0; i < 3; ++i)  
    {
        
        output[i].Position = input[i].Position;
        output[i].Coord = input[i].Coord;
        output[i].Albedo = input[i].Albedo;
        output[i].Normal = input[i].Normal;
        output[i].Emissive = input[i].Emissive;
    
        outputStream.Append(output[i]);    
    }
}