// Intel Open Image Denoise composite and temporal stabilization.
//
// Reads the denoised linear radiance (written by OIDN into a shared fp16 buffer),
// stabilizes it temporally using motion vectors, scale-invariant linear depth
// disocclusion rejection, and dynamic per-pixel sample count tracking, and then
// re-encodes it before compositing with raster graphics and motion vectors
// into the game's render targets.

cbuffer OIDNParams : register(b0)
{
	uint2 RenderSize;
	float ColorEncodeExponent;
	float DepthDisocclusionThreshold;

	uint TemporalEnabled;
	float HistoryWeight;
	uint HistoryValid;
	float MaxAccumulationFrames;

	float4 pad0;

	float4 CameraData;
};

StructuredBuffer<uint2> DenoisedColor : register(t0);
Texture2D<float4> PathTracingColor : register(t1);
Texture2D<float4> MotionVectors : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> HistoryColor : register(t4);
Texture2D<float> HistoryDepth : register(t5);

RWTexture2D<float4> MainOutput : register(u0);
RWTexture2D<float2> MotionVectorOutput : register(u1);
RWTexture2D<float4> OutputHistoryColor : register(u2);
RWTexture2D<float> OutputHistoryDepth : register(u3);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

float2 UnpackHalf2(uint packed)
{
	return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

// Convert raw hardware depth [0, 1] to view-space linear depth using CameraData (x=far, y=near, z=far-near, w=far*near)
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
	const uint2 packed = DenoisedColor[pixelIndex];
	float3 currentLinear = float3(UnpackHalf2(packed.x), UnpackHalf2(packed.y).x);
	if (any(isnan(currentLinear)) || any(isinf(currentLinear)))
		currentLinear = 0.0f.xxx;
	currentLinear = max(currentLinear, 0.0f.xxx);

	const float currDepth = DepthTexture[id];
	const float2 texSize = float2(RenderSize);
	const float2 currUV = (float2(id) + 0.5f) / texSize;

	// Reprojection using screen-space motion vectors
	const float4 mvSample = MotionVectors[id];
	const float2 velocity = mvSample.rg;
	const float2 prevUV = currUV + velocity;

	const bool outOfBounds = any(prevUV < 0.0f) || any(prevUV > 1.0f);

	float3 stabilizedLinear = currentLinear;
	float currentSampleCount = 1.0f;

	if (TemporalEnabled != 0 && HistoryValid != 0 && !outOfBounds)
	{
		// Sample previous history color
		float4 historySample = HistoryColor.SampleLevel(LinearSampler, prevUV, 0);

		if (any(isnan(historySample.rgb)) || any(isinf(historySample.rgb)))
			historySample.rgb = currentLinear;

		// Disocclusion test using scale-invariant linear depth disparity
		const float prevDepth = HistoryDepth.SampleLevel(PointSampler, prevUV, 0);
		const float currLinearDepth = GetLinearDepth(currDepth);
		const float prevLinearDepth = GetLinearDepth(prevDepth);
		const float depthDiff = abs(currLinearDepth - prevLinearDepth);
		const float minDepth = min(currLinearDepth, prevLinearDepth);
		const float depthThreshold = DepthDisocclusionThreshold * max(minDepth, 1.0f);
		const bool disoccluded = depthDiff > depthThreshold;

		// Sample previous history accumulation count using point sampling
		const float prevSampleCount = HistoryColor.SampleLevel(PointSampler, prevUV, 0).a;

		// Dynamic accumulation: increment sample count up to MaxAccumulationFrames
		if (!disoccluded)
		{
			currentSampleCount = min(max(prevSampleCount, 1.0f) + 1.0f, max(MaxAccumulationFrames, 1.0f));
			float alpha = 1.0f / currentSampleCount;
			const float alphaMin = saturate(1.0f - HistoryWeight);
			alpha = max(alpha, alphaMin);
			stabilizedLinear = lerp(historySample.rgb, currentLinear, alpha);
		}
		else
		{
			currentSampleCount = 1.0f;
			stabilizedLinear = currentLinear;
		}
	}

	// Update ping-pong history buffers with stabilized color and new sample count
	OutputHistoryColor[id] = float4(stabilizedLinear, currentSampleCount);
	OutputHistoryDepth[id] = currDepth;

	// Re-encode linear radiance with color encode exponent
	float3 color = pow(abs(stabilizedLinear), ColorEncodeExponent);

	// Blend with raster and path tracing
	float4 mainRaster = MainOutput[id];
	if (any(isnan(mainRaster.rgb)) || any(isinf(mainRaster.rgb)))
		mainRaster.rgb = 0.0f.xxx;

	float4 mainPT = PathTracingColor[id];
	const float blend = saturate(mainPT.a);
	const float3 mainFinal = lerp(mainRaster.rgb, color, blend);
	MainOutput[id] = float4(mainFinal, mainRaster.a);

	// Composite motion vectors
	const float2 mvRaster = MotionVectorOutput[id];
	const float2 mvFinal = lerp(mvRaster, velocity, blend);
	MotionVectorOutput[id] = mvFinal;
}
