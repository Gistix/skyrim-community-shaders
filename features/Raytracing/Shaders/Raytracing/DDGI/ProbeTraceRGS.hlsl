/*
 * DDGI Probe Ray Tracing - Ray Generation Shader
 *
 * Traces rays from probe positions to gather radiance and distance data.
 * Output is used by RTXGI's probe update compute shaders.
 */

#include "Raytracing/Includes/Types.hlsli"
#include "Raytracing/Includes/Registers.hlsli"
#include "Raytracing/Includes/Common.hlsli"
#include "Raytracing/Includes/RT/CommonRT.hlsli"
#include "Raytracing/Includes/RT/IndirectPayload.hlsli"
#include "Raytracing/Includes/RT/Rays.hlsli"
#include "Raytracing/Includes/RT/Shading.hlsli"

// Define HLSL for RTXGI headers
#define HLSL 1

// Coordinate system: Left-handed Y-up (Skyrim)
#ifndef RTXGI_COORDINATE_SYSTEM
#define RTXGI_COORDINATE_SYSTEM 0
#endif

// DDGI SDK includes
#include "Common.hlsl"
#include "include/Common.hlsl"
#include "include/ProbeIndexing.hlsl"
#include "include/ProbeRayCommon.hlsl"

// DDGI-specific resources
ConstantBuffer<DDGIVolumeDescGPUPacked> DDGIVolume : register(b2);

// Output: Probe ray data (radiance RGB, hit distance A)
// This is a Texture2DArray where:
// - X = ray index (0 to raysPerProbe-1)
// - Y = probe index within a slice (0 to probesPerPlane-1)
// - Z = slice index (Y-planes in Y-up coordinate system)
RWTexture2DArray<float4> ProbeRayData : register(u3);

// Get a ray direction for a probe ray using spherical Fibonacci
float3 GetProbeRayDirection(int rayIndex, int numRays, float4 rayRotation)
{
    // Use RTXGI's spherical fibonacci for uniform sphere distribution
    float3 direction = RTXGISphericalFibonacci((float)rayIndex, (float)numRays);

    // Apply rotation for temporal variation
    direction = RTXGIQuaternionRotate(direction, rayRotation);

    return normalize(direction);
}

[shader("raygeneration")]
void main()
{
    // Dispatch dimensions match RayData texture layout:
    // X = rayIndex (0 to raysPerProbe-1)
    // Y = probeIndexInSlice (0 to probesPerPlane-1)
    // Z = sliceIndex (0 to numSlices-1)
    uint rayIndex = DispatchRaysIndex().x;
    uint probeIndexInSlice = DispatchRaysIndex().y;
    uint sliceIndex = DispatchRaysIndex().z;

    // Unpack volume descriptor
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolume);

    // Calculate linear probe index from slice coordinates
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);
    uint probeIndex = sliceIndex * probesPerPlane + probeIndexInSlice;

    // Get probe grid coordinates from linear index
    int3 probeCoords = DDGIGetProbeCoords(probeIndex, volume);

    // Get probe world position (without relocation offset for ray origin)
    float3 probeOrigin = DDGIGetProbeWorldPosition(probeCoords, volume);

    // Get ray direction using spherical Fibonacci distribution
    float3 rayDirection = GetProbeRayDirection(rayIndex, volume.probeNumRays, volume.probeRayRotation);

    // Initialize ray
    RayDesc ray;
    ray.Origin = probeOrigin;
    ray.Direction = rayDirection;
    ray.TMin = 0.0f;
    ray.TMax = volume.probeMaxRayDistance;

    // Trace the ray
    IndirectPayload payload;
    payload.color = float4(0, 0, 0, -1.0f);  // RGB = radiance, A = hit distance (-1 = miss)
    payload.data = PayloadData::Create(false, 0, InitRandomSeed(uint2(rayIndex, probeIndex), DispatchRaysDimensions().xy, Frame.FrameCount));

    TraceRay(
        Scene,
        RAY_FLAG_NONE,
        0xFF,  // Instance mask
        0,     // Hit group index
        1,     // Hit group stride
        0,     // Miss shader index
        ray,
        payload
    );

    // Prepare output
    float3 radiance = payload.color.rgb;
    float hitDistance = payload.color.a;

    // If we hit the sky, use sky color but mark as max distance
    if (hitDistance < 0.0f)
    {
        // Sample sky hemisphere for sky color
        float2 skyUV = DirectionToHemisphereUV(rayDirection);
        radiance = SkyHemisphere.SampleLevel(BaseSampler, skyUV, 0).rgb;
        hitDistance = volume.probeMaxRayDistance;
    }

    // Store probe ray data directly using dispatch indices
    // RayData texture layout: [rayIndex, probeIndexInSlice, sliceIndex]
    // This matches DispatchRaysIndex() directly since we dispatch with matching dimensions
    uint3 outputCoord = uint3(rayIndex, probeIndexInSlice, sliceIndex);
    ProbeRayData[outputCoord] = float4(radiance, hitDistance);
}
