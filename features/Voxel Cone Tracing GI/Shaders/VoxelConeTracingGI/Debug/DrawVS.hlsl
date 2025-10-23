#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

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

cbuffer VCTGICB : register(b0)
{
	VoxelConeTracingGI::ConstantBuffer VCTGI;
}

VS_OUT main(VS_IN input)
{
    VS_OUT output;

    uint clipIdx = 0;
    VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[clipIdx];
    
    float3 voxelPos = clipmap.Min + ((input.Coord * VCTGI.ResRcp * clipmap.Size) * RCP_SCALE);

    // Translate cube vertex by voxel position
    float3 worldPos = ((input.Position * clipmap.Size * VCTGI.ResRcp) * RCP_SCALE) + voxelPos;

    output.Position = mul(FrameBuffer::CameraViewProj[0], float4(worldPos - FrameBuffer::CameraPosAdjust[0].xyz, 1.0f));

    output.Coord = input.Coord;
    output.Albedo = input.Albedo;
    output.Normal = input.Normal;
    output.Emission = input.Emission;
    
    return output;
}