#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Raytracing/Includes/Common.hlsli"

Texture2D<unorm float4> NormalSmoothness : register(t0);

RWTexture2D<float4> NormalRoughness : register(u0);

[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= DynamicResolution))
		return;

	const float3 normalSmoothness = NormalSmoothness[id].xyz;
	const float3 normalWS = normalize(ViewToWorldVector(GBuffer::DecodeNormal(normalSmoothness.xy), FrameBuffer::CameraViewInverse));
	NormalRoughness[id] = float4(normalWS, 1.0f - saturate(normalSmoothness.z));
}
