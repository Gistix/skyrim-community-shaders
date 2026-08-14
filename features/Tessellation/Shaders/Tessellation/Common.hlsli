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
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

struct PatchConstantOutput
{
	float tessFactor[3] : SV_TessFactor;
	float insideTessFactor : SV_InsideTessFactor;
};

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
