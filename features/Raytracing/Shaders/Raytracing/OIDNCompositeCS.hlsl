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

// 5-tap Catmull-Rom bicubic history reconstruction (center + cross taps).
// The negative lobes of Catmull-Rom cancel out the high-frequency attenuation
// of bilinear filtering, preventing the compounded blur that occurs during camera motion.
float3 SampleHistoryCatmullRom(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texSize)
{
	float2 samplePos = uv * texSize;
	float2 tc = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - tc;
	float2 f2 = f * f;
	float2 f3 = f2 * f;

	float2 w0 = f2 - 0.5f * (f3 + f);
	float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
	float2 w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
	float2 w3 = 0.5f * (f3 - f2);

	float2 w12 = w1 + w2;
	float2 tc0 = (tc - 1.0f) / texSize;
	float2 tc12 = (tc + w2 / w12) / texSize;
	float2 tc3 = (tc + 2.0f) / texSize;

	float weightTop    = w12.x * w0.y;
	float weightLeft   = w0.x * w12.y;
	float weightCenter = w12.x * w12.y;
	float weightRight  = w3.x * w12.y;
	float weightBottom = w12.x * w3.y;

	float weightSum = weightTop + weightLeft + weightCenter + weightRight + weightBottom;

	float3 result =
		tex.SampleLevel(samp, float2(tc12.x, tc0.y), 0).rgb * weightTop +
		tex.SampleLevel(samp, float2(tc0.x, tc12.y), 0).rgb * weightLeft +
		tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0).rgb * weightCenter +
		tex.SampleLevel(samp, float2(tc3.x, tc12.y), 0).rgb * weightRight +
		tex.SampleLevel(samp, float2(tc12.x, tc3.y), 0).rgb * weightBottom;

	return max(result / weightSum, 0.0f.xxx);
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
		// Sample previous history color using 5-tap Catmull-Rom bicubic filter to preserve texture sharpness in motion
		float3 historyColor = SampleHistoryCatmullRom(HistoryColor, LinearSampler, prevUV, texSize);

		if (any(isnan(historyColor)) || any(isinf(historyColor)))
			historyColor = currentLinear;

		// Disocclusion test using sublinear scale-invariant linear depth disparity
		// 4-tap footprint gather ensures closest occluding edge is detected
		const float4 prevDepths = HistoryDepth.Gather(PointSampler, prevUV);
		const float prevDepth = min(min(prevDepths.x, prevDepths.y), min(prevDepths.z, prevDepths.w));
		const float currLinearDepth = GetLinearDepth(currDepth);
		const float prevLinearDepth = GetLinearDepth(prevDepth);
		const float depthDiff = abs(currLinearDepth - prevLinearDepth);
		const float minDepth = min(currLinearDepth, prevLinearDepth);
		const float depthThreshold = DepthDisocclusionThreshold * sqrt(500.0f * max(minDepth, 10.0f));
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
			stabilizedLinear = lerp(historyColor, currentLinear, alpha);
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
