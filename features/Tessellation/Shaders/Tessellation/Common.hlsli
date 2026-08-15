/*
 * Tessellation feature shared definitions.
 *
 * Included by the Lighting/Utility hull and domain shaders. Contains the
 * common constant buffer, patch-constant output struct, and interpolation
 * helpers. The per-shader input/output structs (which must mirror each
 * base shader's VS_OUTPUT) stay in the individual shader files.
 */

#ifndef TESSELLATION_COMMON_HLSLI
#define TESSELLATION_COMMON_HLSLI

#define INPUT_PATCH_SIZE 3

// Feature constant buffer, bound to HS/DS slot 0 by Tessellation.
cbuffer TessellationParams : register(b0)
{
	float Scale;
	float Factor;
	float Offset;
	float FadeStart;
	float FadeDistance;
	float Density;
	float MaterialScale;
	uint Pad1;
};

struct PatchConstantOutput
{
	float tessFactor[3] : SV_TessFactor;
	float insideTessFactor : SV_InsideTessFactor;
};

// Fades tessellation from FadeStart to FadeStart + FadeDistance (smoothstep).
float DistanceFade(float a_distance)
{
	float fadeRange = max(FadeDistance, 0.001);
	float fade = 1.0 - saturate((a_distance - FadeStart) / fadeRange);
	return fade * fade * (3.0 - 2.0 * fade);
}

// Per-edge tessellation factor from projected screen-space edge length and
// distance fade. Falls back to the max factor when an endpoint is behind the
// camera (clip w <= 0) to avoid NaN from the perspective divide.
float EdgeTessFactor(float4 a_p0, float4 a_p1, float3 a_w0, float3 a_w1)
{
	if (a_p0.w <= 0.0 || a_p1.w <= 0.0)
		return Factor;
	float2 ndc0 = a_p0.xy / a_p0.w;
	float2 ndc1 = a_p1.xy / a_p1.w;
	float edgeLength = length(ndc1 - ndc0) * 0.5 * Density;
	float fade = min(DistanceFade(length(a_w0)), DistanceFade(length(a_w1)));
	return clamp(edgeLength * fade, 1.0, Factor);
}

// Computes adaptive edge factors (screen-space length * distance fade) and an
// inside factor from their average. Shared edges get identical factors from
// both adjacent patches, so the result stays watertight with integer
// partitioning.
PatchConstantOutput CalculateTessFactors(float4 a_p0, float4 a_p1, float4 a_p2,
	float3 a_w0, float3 a_w1, float3 a_w2)
{
	PatchConstantOutput output;
	float e0 = EdgeTessFactor(a_p0, a_p1, a_w0, a_w1);
	float e1 = EdgeTessFactor(a_p1, a_p2, a_w1, a_w2);
	float e2 = EdgeTessFactor(a_p2, a_p0, a_w2, a_w0);
	output.tessFactor[0] = e0;
	output.tessFactor[1] = e1;
	output.tessFactor[2] = e2;
	output.insideTessFactor = clamp((e0 + e1 + e2) / 3.0, 1.0, Factor);
	return output;
}

float Interpolate1(float3 a_barycentric, float a0, float a1, float a2)
{
	return a_barycentric.x * a0 + a_barycentric.y * a1 + a_barycentric.z * a2;
}

float2 Interpolate2(float3 a_barycentric, float2 a0, float2 a1, float2 a2)
{
	return a_barycentric.x * a0 + a_barycentric.y * a1 + a_barycentric.z * a2;
}

float3 Interpolate3(float3 a_barycentric, float3 a0, float3 a1, float3 a2)
{
	return a_barycentric.x * a0 + a_barycentric.y * a1 + a_barycentric.z * a2;
}

float4 Interpolate4(float3 a_barycentric, float4 a0, float4 a1, float4 a2)
{
	return a_barycentric.x * a0 + a_barycentric.y * a1 + a_barycentric.z * a2;
}

#endif  // TESSELLATION_COMMON_HLSLI
