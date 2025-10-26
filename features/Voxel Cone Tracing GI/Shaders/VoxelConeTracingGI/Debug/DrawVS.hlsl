#include "Common/Game.hlsli"
#include "Common/FrameBuffer.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

struct VS_IN
{
    float3 Position : POSITION;
};

struct VS_OUT
{
    float4 Position : SV_POSITION;
    nointerpolation uint3 Coord     : TEXCOORD0;   
    nointerpolation float3 Albedo   : COLOR0;
    nointerpolation float3 Normal   : NORMAL;
    nointerpolation float3 Emissive : COLOR1;
    //nointerpolation float3 VoxelPosition : TEXCOORD1;       
};

cbuffer VCTGICB : register(b0)
{
	VoxelConeTracingGI::ConstantBuffer VCTGI;
}

cbuffer ClipmapCB : register(b1)
{
    VoxelConeTracingGI::ClipmapConstantBuffer Clipmap;
};

StructuredBuffer<Voxel> ClipmapVoxels : register(t0);

VS_OUT main(VS_IN input, uint instanceID : SV_InstanceID)
{
    VS_OUT output;

    VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[Clipmap.Index];
    
    Voxel voxel = ClipmapVoxels[instanceID];
    
    float3 voxelPos = clipmap.Min + ((voxel.Coord * VCTGI.ResRcp * clipmap.Size) * RCP_SCALE);

    // Scale and translate cube vertex
    float3 worldPos = ((input.Position * clipmap.Size * VCTGI.ResRcp) * RCP_SCALE) + voxelPos;

    output.Position = mul(FrameBuffer::CameraViewProj[0], float4(worldPos - FrameBuffer::CameraPosAdjust[0].xyz, 1.0f));

    output.Coord = voxel.Coord;
    output.Albedo = voxel.Albedo;
    output.Normal = voxel.Normal;
    output.Emissive = voxel.Emissive;
    //output.VoxelPosition = voxelPos;
    
    return output;
}