#include "Upscaling/UpscaleVS.hlsl"

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

float4 main(VS_OUTPUT input) : SV_Target
{
	return InputTexture.Sample(LinearSampler, input.TexCoord.xy);
}