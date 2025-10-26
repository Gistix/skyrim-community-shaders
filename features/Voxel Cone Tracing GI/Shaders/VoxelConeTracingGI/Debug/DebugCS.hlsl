#define COMPUTESHADER

#include "Common/SharedData.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

cbuffer DebugCB : register(b0)
{
	float4 NDCToView;

	float2 RcpFrameDim;
	float2 _pad0;

	float3 Coord2Cell;
	uint _pad1;

	float3 Min;
	uint pad2;
}

Texture2D<unorm float> Depth : register(t0);
Texture3D<half4> LOD0 : register(t1);

RWTexture2D<half4> Debug : register(u0);

[numthreads(8, 8, 1)]
void main(int2 id: SV_DispatchThreadID)
{
	const float2 uv = (id + .5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 positionSS = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	if (any(positionSS < 0) || any(positionSS > 1))
		return;

	const float depth = Depth[id];
	const float depthLinear = SharedData::GetScreenDepth(depth);

	float3 positionVS = ScreenToViewPosition(positionSS, depthLinear, NDCToView);

	float3 positionWS = ViewToWorldPosition(positionVS, FrameBuffer::CameraViewInverse[eyeIndex]) + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	uint3 uvw = (uint3)floor((positionWS - Min) * Coord2Cell);

	half4 lod0Color = LOD0[uvw];
	Debug[id] = half4(lod0Color.rgb, 1.0);
}