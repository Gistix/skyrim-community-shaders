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

// Output 16-channel planar NCHW float tensor buffer.
// Layout: [1, 16, Height, Width] -> channel * (Width * Height) + pixelIndex.
RWStructuredBuffer<float> OutputTensor : register(u0);

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

	const uint pixelCount = RenderSize.x * RenderSize.y;

	// Planar NCHW layout: [1, 16, Height, Width] -> channel * pixelCount + pixelIndex
	OutputTensor[0 * pixelCount + pixelIndex] = demodDiffuse.r;
	OutputTensor[1 * pixelCount + pixelIndex] = demodDiffuse.g;
	OutputTensor[2 * pixelCount + pixelIndex] = demodDiffuse.b;
	OutputTensor[3 * pixelCount + pixelIndex] = demodSpecular.r;
	OutputTensor[4 * pixelCount + pixelIndex] = demodSpecular.g;
	OutputTensor[5 * pixelCount + pixelIndex] = demodSpecular.b;
	OutputTensor[6 * pixelCount + pixelIndex] = normLinearDepth;
	OutputTensor[7 * pixelCount + pixelIndex] = normal.x;
	OutputTensor[8 * pixelCount + pixelIndex] = normal.y;
	OutputTensor[9 * pixelCount + pixelIndex] = normal.z;
	OutputTensor[10 * pixelCount + pixelIndex] = roughness;
	OutputTensor[11 * pixelCount + pixelIndex] = motionVectors.x;
	OutputTensor[12 * pixelCount + pixelIndex] = motionVectors.y;
	OutputTensor[13 * pixelCount + pixelIndex] = curvature;
	OutputTensor[14 * pixelCount + pixelIndex] = hitDist;
	OutputTensor[15 * pixelCount + pixelIndex] = confidence;
}
