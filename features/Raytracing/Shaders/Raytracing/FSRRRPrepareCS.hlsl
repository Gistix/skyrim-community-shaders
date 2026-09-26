// FidelityFX Ray Regeneration (MLD / FSR-RR) input preparation compute shader.
//
// Packs path traced radiance, demodulated diffuse/specular albedo, normals, roughness,
// screen-space motion vectors, linear view depth, surface curvature, and hit distance / AO
// into a 16-channel FP16 tensor buffer (32 bytes per pixel) for neural denoiser inference.

#include "Common/FrameBuffer.hlsli"

cbuffer FSRRRPrepareParams : register(b0)
{
	uint2 RenderSize;
	float ColorDecodeExponent;
	float Padding;

	float4 CameraData;
};

Texture2D<float4> InputColor : register(t0);          // Shared PT main radiance
Texture2D<float4> InputDiffuseAlbedo : register(t1);  // Raytracing diffuse albedo
Texture2D<float4> InputSpecularAlbedo : register(t2); // Raytracing specular albedo
Texture2D<float4> InputNormal : register(t3);         // World normal (xyz) + roughness (w)
Texture2D<float4> InputMotionVectors : register(t4);  // Screen motion vectors
Texture2D<float>  InputDepth : register(t5);          // Hardware depth
Texture2D<float>  InputHitDist : register(t6);        // Hit distance / AO guide

// Output 16-channel FP16 tensor buffer.
// Each pixel occupies 32 bytes (16 x fp16 = 8 uints = 2 x uint4).
RWStructuredBuffer<uint4> OutputTensor : register(u0);

uint PackHalf2(float2 value)
{
	return (f32tof16(value.x) & 0xFFFFu) | (f32tof16(value.y) << 16);
}

float GetLinearDepth(float rawDepth)
{
	return CameraData.w / max(-rawDepth * CameraData.z + CameraData.x, 1e-7f);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= RenderSize))
		return;

	const uint pixelIndex = id.y * RenderSize.x + id.x;

	// 1. Color / Radiance: linearized display-referred path tracing color
	float4 ptColor = InputColor[id];
	if (any(isnan(ptColor.rgb)) || any(isinf(ptColor.rgb)))
		ptColor.rgb = 0.0f.xxx;

	float3 colorLinear = pow(abs(ptColor.rgb), ColorDecodeExponent);

	// 2. Diffuse & Specular Albedo
	float3 diffAlbedo = InputDiffuseAlbedo[id].rgb;
	if (any(isnan(diffAlbedo)) || any(isinf(diffAlbedo)))
		diffAlbedo = 0.0f.xxx;
	diffAlbedo = saturate(diffAlbedo);

	float3 specAlbedo = InputSpecularAlbedo[id].rgb;
	if (any(isnan(specAlbedo)) || any(isinf(specAlbedo)))
		specAlbedo = 0.0f.xxx;
	specAlbedo = saturate(specAlbedo);

	// Demodulate radiance by diffuse albedo (avoiding division by zero)
	float3 demodDiffuse = colorLinear / max(diffAlbedo + 0.02f, 0.02f);
	float3 demodSpecular = specAlbedo;

	// 3. Normal & Roughness: FSR-RR operates on normalized World Space normals
	float4 normalSample = InputNormal[id];
	float3 normalWS = normalSample.xyz;
	float roughness = saturate(normalSample.w);

	const float lenSq = dot(normalWS, normalWS);
	float3 normal = (lenSq > 1e-4f && !isnan(lenSq) && !isinf(lenSq)) ? normalize(normalWS) : float3(0.0f, 0.0f, 1.0f);

	// 4. Linear Depth
	float rawDepth = InputDepth[id];
	float linearDepth = GetLinearDepth(rawDepth);
	float normLinearDepth = saturate(linearDepth / max(CameraData.x, 1e-4f));

	// 5. Motion Vectors
	float4 mvSample = InputMotionVectors[id];
	float2 motionVectors = mvSample.xy;
	if (any(isnan(motionVectors)) || any(isinf(motionVectors)))
		motionVectors = 0.0f.xx;

	// 6. Surface Curvature: computed from screen-space finite difference of normals
	int2 leftPos = max(int2(id) - int2(1, 0), int2(0, 0));
	int2 rightPos = min(int2(id) + int2(1, 0), int2(RenderSize) - int2(1, 1));
	int2 upPos = max(int2(id) - int2(0, 1), int2(0, 0));
	int2 downPos = min(int2(id) + int2(0, 1), int2(RenderSize) - int2(1, 1));

	float3 nL = InputNormal[leftPos].xyz;
	float3 nR = InputNormal[rightPos].xyz;
	float3 nU = InputNormal[upPos].xyz;
	float3 nD = InputNormal[downPos].xyz;

	float curvature = saturate(0.25f * (length(nR - nL) + length(nD - nU)));

	// 7. Hit Distance / AO guide
	float hitDist = saturate(InputHitDist[id]);
	if (isnan(hitDist) || isinf(hitDist))
		hitDist = 1.0f;

	// 8. Confidence indicator
	float confidence = 1.0f;

	// Pack 16 channels into 8 uints (2 x uint4)
	// Channels:
	//  0, 1: Demodulated Diffuse R, G
	//  2, 3: Demodulated Diffuse B, Specular R
	//  4, 5: Specular G, B
	//  6, 7: Linear Depth, Normal X
	//  8, 9: Normal Y, Normal Z
	// 10,11: Roughness, Motion Vector X
	// 12,13: Motion Vector Y, Curvature
	// 14,15: Hit Distance / AO, Confidence
	uint u0 = PackHalf2(demodDiffuse.rg);
	uint u1 = PackHalf2(float2(demodDiffuse.b, demodSpecular.r));
	uint u2 = PackHalf2(demodSpecular.gb);
	uint u3 = PackHalf2(float2(normLinearDepth, normal.x));
	uint u4 = PackHalf2(normal.yz);
	uint u5 = PackHalf2(float2(roughness, motionVectors.x));
	uint u6 = PackHalf2(float2(motionVectors.y, curvature));
	uint u7 = PackHalf2(float2(hitDist, confidence));

	OutputTensor[pixelIndex * 2 + 0] = uint4(u0, u1, u2, u3);
	OutputTensor[pixelIndex * 2 + 1] = uint4(u4, u5, u6, u7);
}
