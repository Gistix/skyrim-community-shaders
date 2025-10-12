#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VS_IN
{
    uint3 Coord     : VOXELCOORD;
    float3 Albedo   : VOXELALBEDO;
    float3 Emission : VOXELEMISSION;
    float3 Normal   : VOXELNORMAL;
};

struct VS_OUT
{
    float4 Position : SV_POSITION;
    float4 Albedo   : COLOR0;
    float4 Emission : COLOR1;
    float4 Normal   : NORMAL;
};

cbuffer VertexCB : register(b0)
{
	float3 Min;
	float Size;
	float SizeInv;
	uint Res;
	float ResInv; 
	float VoxelSize;
};

ByteAddressBuffer VoxelAccumCount : register(t0);

VS_OUT main(VS_IN input)
{
    VS_OUT output;

    uint3 coord = input.Coord;

    float2 pixelCoord;
    pixelCoord.x = (coord.x + coord.z * Res) + 0.5f;
    pixelCoord.y = coord.y + 0.5f;    
    
    float ndcX = (pixelCoord.x / (Res * Res)) * 2.0f - 1.0f;
    float ndcY = (pixelCoord.y * ResInv) * 2.0f - 1.0f;

    output.Position = float4(ndcX, ndcY, 0.0f, 1.0f);

    uint index = coord.x + (coord.y * Res) + (coord.z * Res * Res);
    uint accumCount = VoxelAccumCount.Load(index * 4);
    
    output.Albedo = float4(input.Albedo, accumCount);
    output.Emission = float4(input.Emission, 1.0f);
    output.Normal = float4(input.Normal, accumCount);

    return output;
}