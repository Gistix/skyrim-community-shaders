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
#include "include/ProbeCommon.hlsl"  // Includes ProbeIndexing, ProbeRayCommon, ProbeOctahedral, ProbeDataCommon

// DDGI-specific resources
// Must match root signature: b1, space4 for DDGI Volume constants
ConstantBuffer<DDGIVolumeDescGPUPacked> DDGIVolume : register(b1, space4);

// Output: Probe ray data (radiance RGB, hit distance A)
RWTexture2DArray<float4> ProbeRayData : register(u3);

// Probe Data texture for relocation offsets (same binding as DDGISampling.hlsli)
Texture2DArray<float4> ProbeData : register(t2, space4);

[shader("raygeneration")]
void main()
{
    // Dispatch dimensions match RayData texture layout
    int rayIndex = DispatchRaysIndex().x;
    int probePlaneIndex = DispatchRaysIndex().y;
    int planeIndex = DispatchRaysIndex().z;

    // Unpack volume descriptor
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolume);

    // Calculate linear probe index
    int probesPerPlane = DDGIGetProbesPerPlane(volume.probeCounts);
    int probeIndex = (planeIndex * probesPerPlane) + probePlaneIndex;

    // Get probe grid coordinates from linear index
    int3 probeCoords = DDGIGetProbeCoords(probeIndex, volume);

    // Adjust probe index for scrolling (if enabled)
    probeIndex = DDGIGetScrollingProbeIndex(probeCoords, volume);

    // Early out: skip tracing for inactive probes (except for fixed rays needed for classification)
    // This optimization avoids wasting GPU time on probes inside geometry or far from surfaces
    float probeState = DDGILoadProbeState(probeIndex, ProbeData, volume);
    if (probeState == RTXGI_DDGI_PROBE_STATE_INACTIVE && rayIndex >= RTXGI_DDGI_NUM_FIXED_RAYS)
        return;

    // Get output coordinates using SDK function
    uint3 outputCoords = DDGIGetRayDataTexelCoords(rayIndex, probeIndex, volume);

    // Get probe world position (with relocation offsets if enabled)
    float3 probeWorldPosition = DDGIGetProbeWorldPosition(probeCoords, volume, ProbeData);

    // Get ray direction using SDK function (handles fixed rays correctly)
    float3 probeRayDirection = DDGIGetProbeRayDirection(rayIndex, volume);

    // Initialize ray
    RayDesc ray;
    ray.Origin = probeWorldPosition;
    ray.Direction = probeRayDirection;
    ray.TMin = 0.01f;  // Prevent self-intersection at probe position
    ray.TMax = volume.probeMaxRayDistance;

    // Trace the ray
    IndirectPayload payload;
    payload.color = float4(0, 0, 0, -1.0f);  // RGB = radiance, A = hit distance (-1 = miss)
    payload.data = PayloadData::Create(false, 0, InitRandomSeed(uint2(rayIndex, probeIndex), DispatchRaysDimensions().xy, Frame.FrameCount));
    payload.hitKind = HIT_KIND_TRIANGLE_FRONT_FACE;  // Default, will be set by ClosestHit shader

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

    float hitDistance = payload.color.a;

    // Ray missed - store sky radiance with large hit distance
    if (hitDistance < 0.0f)
    {
        // Convert ray direction to hemisphere UV for sky sampling
        float3 dir = normalize(probeRayDirection);
        float2 skyUV = float2(
            atan2(dir.x, dir.z) / (2.0f * Math::PI) + 0.5f,
            acos(saturate(dir.y)) / Math::PI
        );
        float3 skyRadiance = SkyHemisphere.SampleLevel(BaseSampler, skyUV, 0).rgb;

        // Use SDK function to store ray miss (uses 1e27f for hit distance)
        DDGIStoreProbeRayMiss(ProbeRayData, outputCoords, volume, skyRadiance);
        return;
    }

    // Backface hit detection - probe is inside geometry
    // This is critical for probe relocation and classification to work correctly
    if (payload.hitKind == HIT_KIND_TRIANGLE_BACK_FACE)
    {
        // Store with negative hit distance (SDK uses -hitT * 0.2f to reduce influence)
        DDGIStoreProbeRayBackfaceHit(ProbeRayData, outputCoords, volume, hitDistance);
        return;
    }

    // Fixed rays (first 32) - only store hit distance for relocation/classification
    if ((volume.probeRelocationEnabled || volume.probeClassificationEnabled) && rayIndex < RTXGI_DDGI_NUM_FIXED_RAYS)
    {
        DDGIStoreProbeRayFrontfaceHit(ProbeRayData, outputCoords, volume, hitDistance);
        return;
    }

    // Store frontface hit with radiance using SDK function
    float3 radiance = payload.color.rgb;
    DDGIStoreProbeRayFrontfaceHit(ProbeRayData, outputCoords, volume, saturate(radiance), hitDistance);
}
