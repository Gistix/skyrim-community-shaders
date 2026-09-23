// Intel Open Image Denoise composite.
//
// Reads the denoised linear radiance (written by OIDN into a shared fp16 buffer)
// and re-encodes it with the raytracing renderer's LLTrueLinearToGamma before
// writing it back into the game main texture.

cbuffer OIDNParams : register(b0)
{
	uint2 RenderSize;
	float ColorEncodeExponent;
	float pad;
};

StructuredBuffer<uint2> DenoisedColor : register(t0);
Texture2D<float4> PathTracingColor : register(t1);

RWTexture2D<float4> MainOutput : register(u0);

float2 UnpackHalf2(uint packed)
{
	return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= RenderSize))
		return;

	const uint pixelIndex = id.y * RenderSize.x + id.x;
	const uint2 packed = DenoisedColor[pixelIndex];
	float3 color = float3(UnpackHalf2(packed.x), UnpackHalf2(packed.y).x);
	color = pow(abs(color), ColorEncodeExponent);

	const float4 mainRaster = MainOutput[id];
	const float blend = PathTracingColor[id].a;
	const float3 mainFinal = lerp(mainRaster.rgb, color, blend);
	MainOutput[id] = float4(mainFinal, mainRaster.a);
}
