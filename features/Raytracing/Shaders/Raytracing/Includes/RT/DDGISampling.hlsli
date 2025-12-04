/*
 * DDGI Sampling Utilities
 *
 * Wrapper for NVIDIA RTXGI-DDGI irradiance sampling.
 * Integrates with existing Skyrim Community Shaders raytracing infrastructure.
 */

#ifndef DDGI_SAMPLING_HLSLI
#define DDGI_SAMPLING_HLSLI

// Define HLSL to skip CPU-only code in RTXGI headers
#define HLSL 1

// Coordinate system: Left-handed Y-up (Skyrim)
#ifndef RTXGI_COORDINATE_SYSTEM
#define RTXGI_COORDINATE_SYSTEM 0
#endif

// Include RTXGI SDK types and functions
// Note: These paths are relative to the Shaders root directory
#include "Raytracing/DDGI/Irradiance.hlsl"

// DDGI Resources - bound in space4 to avoid conflicts with existing bindings
Texture2DArray<float4> DDGIProbeIrradiance     : register(t0, space4);
Texture2DArray<float4> DDGIProbeDistance       : register(t1, space4);
Texture2DArray<float4> DDGIProbeData           : register(t2, space4);
SamplerState DDGIBilinearSampler               : register(s1, space4);

// Volume constants (packed format for efficiency)
ConstantBuffer<DDGIVolumeDescGPUPacked> DDGIVolumeConstants : register(b1, space4);

/**
 * Sample diffuse irradiance from DDGI probe volume.
 *
 * @param worldPosition  World-space position to sample at
 * @param surfaceNormal  Surface normal at the sampling position (normalized)
 * @param viewDirection  View direction from surface to camera (normalized)
 * @return Diffuse irradiance (pre-multiplied by PI for direct use)
 */
float3 SampleDDGIIrradiance(float3 worldPosition, float3 surfaceNormal, float3 viewDirection)
{
    // Unpack the volume descriptor
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolumeConstants);

    // Setup resource bindings
    DDGIVolumeResources resources;
    resources.probeIrradiance = DDGIProbeIrradiance;
    resources.probeDistance = DDGIProbeDistance;
    resources.probeData = DDGIProbeData;
    resources.bilinearSampler = DDGIBilinearSampler;

    // Compute surface bias to avoid self-shadowing artifacts
    float3 surfaceBias = DDGIGetSurfaceBias(surfaceNormal, viewDirection, volume);

    // Sample irradiance from probe grid
    float3 irradiance = DDGIGetVolumeIrradiance(
        worldPosition,
        surfaceBias,
        surfaceNormal,  // Sample in normal direction for diffuse
        volume,
        resources
    );

    return irradiance;
}

/**
 * Get blend weight for a position within the DDGI volume.
 * Useful for blending between DDGI and fallback lighting.
 *
 * @param worldPosition  World-space position to check
 * @return Weight in [0, 1] where 1 = fully inside volume
 */
float GetDDGIVolumeWeight(float3 worldPosition)
{
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolumeConstants);
    return DDGIGetVolumeBlendWeight(worldPosition, volume);
}

/**
 * Sample DDGI irradiance with automatic fallback blending.
 * Uses volume blend weight to fade out near edges.
 *
 * @param worldPosition   World-space position
 * @param surfaceNormal   Surface normal (normalized)
 * @param viewDirection   View direction (normalized)
 * @param fallbackColor   Color to blend towards outside volume
 * @return Blended irradiance
 */
float3 SampleDDGIIrradianceWithFallback(
    float3 worldPosition,
    float3 surfaceNormal,
    float3 viewDirection,
    float3 fallbackColor)
{
    float weight = GetDDGIVolumeWeight(worldPosition);

    if (weight <= 0.0f)
        return fallbackColor;

    float3 ddgiIrradiance = SampleDDGIIrradiance(worldPosition, surfaceNormal, viewDirection);

    return lerp(fallbackColor, ddgiIrradiance, weight);
}

#endif // DDGI_SAMPLING_HLSLI
