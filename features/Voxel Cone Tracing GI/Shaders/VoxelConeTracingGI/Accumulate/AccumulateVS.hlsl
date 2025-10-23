#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

struct VS_IN
{
    uint3 Coord    : VOXELCOORD;
    float3 Albedo   : VOXELALBEDO;
    float3 Normal   : VOXELNORMAL;
    float3 Emissive : VOXELEMISSIVE;    
};

struct VS_OUT
{
    float4 Position : SV_POSITION;
    float4 Albedo   : COLOR0;
    float4 Normal   : NORMAL;
    float4 Emissive : COLOR1;    
};

cbuffer VCTGICB : register(b0)
{
	VoxelConeTracingGI::ConstantBuffer VCTGI;
};

VS_OUT main(VS_IN input)
{
    VS_OUT output;

    uint3 coord = input.Coord;

    float2 pixelCoord;
    pixelCoord.x = (coord.x + coord.z * VCTGI.Res) + 0.5f;
    pixelCoord.y = coord.y + 0.5f;    
    
    float ndcX = (pixelCoord.x / (VCTGI.Res * VCTGI.Res)) * 2.0f - 1.0f;
    float ndcY = ((pixelCoord.y * VCTGI.ResRcp) * RCP_SCALE) * 2.0f - 1.0f;

    output.Position = float4(ndcX, -ndcY, 0.0f, 1.0f);

    output.Albedo = float4(input.Albedo, 1.0);
    output.Normal = float4(input.Normal, 1.0);
    output.Emissive = float4(input.Emissive, 0.0f);
    
    return output;
}