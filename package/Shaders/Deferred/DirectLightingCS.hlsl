
#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"

Texture2D<float3> AlbedoTexture : register(t0);
Texture2D<float3> NormalRoughnessTexture : register(t1);
Texture2D<float3> EmissiveTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);

RWTexture2D<float4> MainRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	const float3 albedo = AlbedoTexture[dispatchID.xy];
	const float3 normalRoughness = NormalRoughnessTexture[dispatchID.xy];
	const float3 normalVS = GBuffer::DecodeNormal(normalRoughness.xy);
	const float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);
    const float3 emissive = EmissiveTexture[dispatchID.xy];
	
	const float depth = DepthTexture[dispatchID.xy];
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;
	
    float ndotl = max(dot(normalWS, float3(0, 0, 1)), 0);
	float3 lighting = ndotl * albedo;
	float3 color = emissive + lighting;
	
	MainRW[dispatchID.xy] = float4(color, 1.0);
}