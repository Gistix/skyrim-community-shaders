/*
 * DDGI Probe Visualization Compute Shader
 *
 * Renders 3D probe spheres using ray-sphere intersection.
 * Shows irradiance sampled from view direction.
 */

#include "Raytracing/Includes/Types.hlsli"
#include "Raytracing/Includes/Registers.hlsli"
#include "Raytracing/Includes/Common.hlsli"

// Define HLSL for RTXGI headers
#define HLSL 1

// Coordinate system: Left-handed Y-up (Skyrim)
#ifndef RTXGI_COORDINATE_SYSTEM
#define RTXGI_COORDINATE_SYSTEM 0
#endif

// DDGI SDK includes
#include "Common.hlsl"
#include "include/Common.hlsl"
#include "include/ProbeCommon.hlsl"

// DDGI Volume constants
ConstantBuffer<DDGIVolumeDescGPUPacked> DDGIVolume : register(b1, space4);

// Probe textures (SRVs in space4)
Texture2DArray<float4> DDGIProbeIrradianceTex : register(t0, space4);
Texture2DArray<float4> DDGIProbeDistanceTex : register(t1, space4);
Texture2DArray<float4> DDGIProbeDataTex : register(t2, space4);

// Bilinear sampler
SamplerState DDGIProbeSampler : register(s1, space4);

// Note: OutputTexture is already defined in Registers.hlsli at register(u0)

// Visualization settings
// Note: Probe spacing is 140 units (~2m)
static const float PROBE_SPHERE_RADIUS = 20.0f;  // Small spheres for clear visualization

// Ray-sphere intersection
// Returns t value (distance along ray) or -1 if no hit
float RaySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float sphereRadius)
{
    float3 oc = rayOrigin - sphereCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;

    if (discriminant < 0.0f)
        return -1.0f;

    float t = -b - sqrt(discriminant);
    if (t < 0.0f)
        t = -b + sqrt(discriminant);  // Inside sphere, use far intersection

    return t;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelCoord = DTid.xy;

    // Get screen dimensions from output texture
    uint width, height;
    OutputTexture.GetDimensions(width, height);

    if (pixelCoord.x >= width || pixelCoord.y >= height)
        return;

    float2 uv = (pixelCoord + 0.5f) / float2(width, height);

    // Unpack volume descriptor
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolume);

    // Get scene depth for occlusion test
    float sceneDepth = DepthTexture[pixelCoord];
    float sceneDepthView = ScreenToViewDepth(sceneDepth, Frame.CameraData);

    // Build ray from camera through pixel
    float3 cameraWS = Frame.Position.xyz;

    // Get far plane position and compute ray direction
    float3 farVS = ScreenToViewPosition(uv, 1.0f, Frame.NDCToView);
    float3 farCS = ViewToWorldPosition(farVS, Frame.ViewInverse);
    float3 farWS = farCS + cameraWS;
    float3 rayDir = normalize(farWS - cameraWS);

    // Find closest probe sphere intersection
    float closestT = 1e10f;
    int closestProbeIndex = -1;
    float3 closestProbePos = float3(0, 0, 0);
    float3 closestHitNormal = float3(0, 0, 0);

    // Get base probe coords at camera position to start search
    int3 cameraProbeCoords = DDGIGetBaseProbeGridCoords(cameraWS, volume);

    // Search in a local region around the camera (performance optimization)
    int searchRadius = 10;  // Check probes within this grid radius

    for (int dz = -searchRadius; dz <= searchRadius; dz++)
    {
        for (int dy = -searchRadius; dy <= searchRadius; dy++)
        {
            for (int dx = -searchRadius; dx <= searchRadius; dx++)
            {
                int3 probeCoords = cameraProbeCoords + int3(dx, dy, dz);

                // Clamp to valid probe range
                if (any(probeCoords < 0) || any(probeCoords >= volume.probeCounts))
                    continue;

                // Get probe world position (with relocation offset if enabled)
                float3 probePos = DDGIGetProbeWorldPosition(probeCoords, volume, DDGIProbeDataTex);

                // Ray-sphere intersection
                float t = RaySphereIntersect(cameraWS, rayDir, probePos, PROBE_SPHERE_RADIUS);

                if (t > 0.0f && t < closestT)
                {
                    // Get camera forward from ViewInverse (third column, negated for -Z)
                    float3 cameraForward = -float3(Frame.ViewInverse[0][2], Frame.ViewInverse[1][2], Frame.ViewInverse[2][2]);

                    // Convert ray distance to approximate view depth
                    // View depth = t * cos(angle between ray and forward)
                    float hitDepthView = t * dot(rayDir, cameraForward);

                    // Only show probe if it's in front of scene geometry (or sky)
                    if (hitDepthView < sceneDepthView || sceneDepth >= SKY_Z)
                    {
                        closestT = t;
                        closestProbeIndex = DDGIGetScrollingProbeIndex(probeCoords, volume);
                        closestProbePos = probePos;
                        float3 hitWS = cameraWS + rayDir * t;
                        closestHitNormal = normalize(hitWS - probePos);
                    }
                }
            }
        }
    }

    // Render the closest probe sphere
    if (closestProbeIndex >= 0)
    {
        // Get probe classification state
        uint3 probeStateTexCoords = DDGIGetProbeTexelCoords(closestProbeIndex, volume);
        float probeState = DDGIProbeDataTex[probeStateTexCoords].w;
        bool isInactive = (probeState >= 0.5f);

        // Sample irradiance in the view direction (from sphere surface toward viewer)
        float3 sampleDir = -rayDir;
        float2 octCoords = DDGIGetOctahedralCoordinates(sampleDir);
        float3 probeUV = DDGIGetProbeUV(closestProbeIndex, octCoords, volume.probeNumIrradianceInteriorTexels, volume);
        float3 irradiance = DDGIProbeIrradianceTex.SampleLevel(DDGIProbeSampler, probeUV, 0).rgb;

        // Decode from gamma-encoded storage
        irradiance = pow(abs(irradiance), volume.probeIrradianceEncodingGamma);

        float3 color;

        if (isInactive)
        {
            // RED for inactive probes (inside geometry)
            // Simple sphere shading with rim highlight
            float rim = 1.0f - saturate(dot(-rayDir, closestHitNormal));
            color = lerp(float3(0.4f, 0.0f, 0.0f), float3(1.0f, 0.2f, 0.2f), rim);
        }
        else
        {
            // Show irradiance with sphere shading
            // Reinhard tone mapping
            float3 tonemapped = irradiance / (irradiance + 1.0f);

            // Add rim highlight for 3D appearance
            float rim = 1.0f - saturate(dot(-rayDir, closestHitNormal));
            float3 rimColor = float3(0.0f, 0.8f, 0.0f);  // Green rim for active probes

            color = lerp(tonemapped * 1.5f, rimColor, rim * rim * 0.5f);
        }

        OutputTexture[pixelCoord] = float4(color, 1.0f);
    }
}
