// FidelityFX Ray Regeneration (MLD / FSR-RR) composite pass.
//
// Reads the 16-channel denoised tensor, re-modulates diffuse and specular radiance
// with albedo guides, and composites into the main render target (blending with raster sky)
// and motion vector buffer.

#include "Common/FrameBuffer.hlsli"

cbuffer FSRRRCompositeParams : register(b0)
{
	uint2 RenderSize;
	float ColorDecodeExponent;
	float Padding;
};

StructuredBuffer<float> DenoisedTensor : register(t0); // 16-channel planar NCHW float tensor
Texture2D<float4> PathTracingColor : register(t1);     // Raw PT radiance (alpha contains blend weight)
Texture2D<float4> InputDiffuseAlbedo : register(t2);   // Diffuse albedo guide
Texture2D<float4> InputSpecularAlbedo : register(t3);  // Specular albedo guide
Texture2D<float4> InputMotionVectors : register(t4);   // Screen motion vectors

RWTexture2D<float4> MainOutput : register(u0);
RWTexture2D<float2> MotionVectorOutput : register(u1);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= RenderSize))
		return;

	const uint pixelIndex = id.y * RenderSize.x + id.x;
	const uint pixelCount = RenderSize.x * RenderSize.y;

	// Planar NCHW layout:
	// Channels 0..2: demodulated diffuse radiance (RGB)
	// Channels 3..5: demodulated specular radiance (RGB)
	float3 denoisedDemodDiffuse = max(float3(
		DenoisedTensor[0 * pixelCount + pixelIndex],
		DenoisedTensor[1 * pixelCount + pixelIndex],
		DenoisedTensor[2 * pixelCount + pixelIndex]
	), 0.0f.xxx);

	float3 denoisedSpecular = max(float3(
		DenoisedTensor[3 * pixelCount + pixelIndex],
		DenoisedTensor[4 * pixelCount + pixelIndex],
		DenoisedTensor[5 * pixelCount + pixelIndex]
	), 0.0f.xxx);

	// Fetch albedo guides
	float3 diffAlbedo = saturate(InputDiffuseAlbedo[id].rgb);
	float3 specAlbedo = saturate(InputSpecularAlbedo[id].rgb);

	// Re-modulate
	float3 diffuseRadiance = denoisedDemodDiffuse * max(diffAlbedo, 0.02f);
	float3 specularRadiance = denoisedSpecular * max(specAlbedo, 0.02f);
	float3 reconstructedRadiance = diffuseRadiance + specularRadiance;

	// Fetch raw path tracing input for alpha blending
	float4 ptSample = PathTracingColor[id];
	float blend = saturate(ptSample.a);

	// Fetch raster sky from game main texture (MainOutput UAV in-out)
	float4 mainRaster = MainOutput[id];
	if (any(isnan(mainRaster.rgb)) || any(isinf(mainRaster.rgb)))
		mainRaster.rgb = 0.0f.xxx;

	// Gamma re-encode to display-referred space and blend with raster
	float3 displayColor = pow(max(reconstructedRadiance, 0.0f.xxx), 1.0f / ColorDecodeExponent);
	const float3 finalMain = lerp(mainRaster.rgb, displayColor, blend);

	MainOutput[id] = float4(finalMain, mainRaster.a);

	// Composite motion vectors
	float4 mvSample = InputMotionVectors[id];
	float2 motion = mvSample.xy;
	const float2 mvRaster = MotionVectorOutput[id];
	const float2 finalMV = lerp(mvRaster, motion, blend);
	MotionVectorOutput[id] = finalMV;
}
