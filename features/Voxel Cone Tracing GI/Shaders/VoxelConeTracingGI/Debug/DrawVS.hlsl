#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VS_IN
{
    float3 Position : POSITION;
    uint3 Coord : VOXELCOORD;
    float3 Albedo : VOXELALBEDO;
    float3 Normal : VOXELNORMAL;
    float3 Emission : VOXELEMISSION;
};

struct VS_OUT
{
    float4 Position : SV_POSITION;
    uint3 Coord : TEXCOORD0;   
    float3 Albedo : COLOR0;
    float3 Normal : NORMAL;
    float3 Emission : COLOR1;
};

cbuffer VertexCB : register(b0)
{
	float3 Min;
	float Size;
	float SizeInv;
	uint Res;
	float ResInv; 
	float pad0;
};

VS_OUT main(VS_IN input)
{
    VS_OUT output;

    float3 voxelPos = Min + (input.Coord * ResInv * Size);

    // Translate cube vertex by voxel position
    float3 worldPos = (input.Position * Size * ResInv) + voxelPos;

    output.Position = mul(FrameBuffer::CameraViewProj[0], float4(worldPos - FrameBuffer::CameraPosAdjust[0].xyz, 1.0f));

    output.Coord = input.Coord;
    output.Albedo = input.Albedo;
    output.Normal = input.Normal;
    output.Emission = input.Emission;
    
    return output;
}