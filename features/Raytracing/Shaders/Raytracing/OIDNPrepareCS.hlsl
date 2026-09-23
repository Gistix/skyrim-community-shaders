// Intel Open Image Denoise input preparation.
//
// Runs as a D3D11 compute pass right after the path tracer / PT composite writes
// the game main texture. It linearizes the display-referred color (inverse of the
// raytracing renderer's LLTrueLinearToGamma) and packs color, albedo and normal
// into tightly packed fp16 buffers (stride 8, Half3 + padding) that are copied
// into the OIDN-owned Vulkan buffers.

cbuffer OIDNParams : register(b0)
{
	uint2 RenderSize;
	float ColorDecodeExponent;
	float pad;
};

Texture2D<float4> InputColor : register(t0);
Texture2D<float4> InputAlbedo : register(t1);
Texture2D<float4> InputNormal : register(t2);

RWStructuredBuffer<uint2> OutputColor : register(u0);
RWStructuredBuffer<uint2> OutputAlbedo : register(u1);
RWStructuredBuffer<uint2> OutputNormal : register(u2);

uint PackHalf2(float2 value)
{
	return (f32tof16(value.x) & 0xFFFFu) | (f32tof16(value.y) << 16);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (any(id >= RenderSize))
		return;

	const uint pixelIndex = id.y * RenderSize.x + id.x;

	// Color: full HDR radiance, converted to the "true linear" space OIDN expects.
	float4 color = InputColor[id];
	if (any(isnan(color.rgb)) || any(isinf(color.rgb)))
		color.rgb = 0.0f.xxx;
	color.rgb = pow(abs(color.rgb), ColorDecodeExponent);
	OutputColor[pixelIndex] = uint2(PackHalf2(color.rg), PackHalf2(float2(color.b, 0.0f)));

	// Albedo: raw diffuse albedo in [0, 1].
	float3 albedo = InputAlbedo[id].rgb;
	if (any(isnan(albedo)) || any(isinf(albedo)))
		albedo = 0.0f.xxx;
	albedo = saturate(albedo);
	OutputAlbedo[pixelIndex] = uint2(PackHalf2(albedo.rg), PackHalf2(float2(albedo.b, 0.0f)));

	// Normal: decoded from the [0, 1] encoding.
	float3 normal = InputNormal[id].rgb * 2.0f - 1.0f;
	const float lenSq = dot(normal, normal);
	normal = (lenSq > 1e-4f && !isnan(lenSq) && !isinf(lenSq)) ? normalize(normal) : float3(0.0f, 0.0f, 1.0f);
	OutputNormal[pixelIndex] = uint2(PackHalf2(normal.rg), PackHalf2(float2(normal.b, 0.0f)));
}
