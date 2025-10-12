#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

struct VS_IN
{
    float3 Position : POSITION;       // cube vertex pos
    uint3 Coord : VOXELCOORD; // instance voxel index
    float3 Albedo : VOXELALBEDO;   // instance color/albedo
    float3 Emission : VOXELEMISSION; // instance emission color
    float3 Normal : VOXELNORMAL;    // instance normal vector
};

struct VS_OUT
{
    float4 Position : SV_POSITION;
    float3 Albedo : COLOR0;
    float3 Emission : COLOR1;
    float3 Normal : NORMAL;  // or another semantic as you want
};

cbuffer VertexCB : register(b0)
{
	float3 Min;
	float Size;
	float SizeInv;
	float Res;
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

    output.Albedo = input.Albedo;
    output.Emission = input.Emission;
    output.Normal = input.Normal;

    return output;
}