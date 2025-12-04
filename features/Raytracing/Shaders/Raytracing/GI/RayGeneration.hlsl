#include "Raytracing/Includes/Types.hlsli"
#include "Raytracing/Includes/Registers.hlsli"

#include "Raytracing/Includes/Common.hlsli"
#include "Raytracing/Includes/RT/CommonRT.hlsli"
#include "Raytracing/Includes/RT/Rays.hlsli"
#include "Raytracing/Includes/RT/Shading.hlsli"

#ifdef SHARC
#include "Raytracing/Includes/RT/SHARC/SharcCommon.hlsli"
#endif

#include "Common/Color.hlsli"

[shader("raygeneration")]
void main()
{
    uint2 idx  = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;

    float2 uv = (idx + 0.5f) / size;

    const float4 normalMetalness = GNMXTexture[idx];

    const half3 geometryNormalVS = DecodeNormal(normalMetalness.xy);
    const float3 geometryNormalWS = normalize(ViewToWorldVector(geometryNormalVS, Frame.ViewInverse));

    const float metalness = Scale01(normalMetalness.z, Frame.Metalness.x, Frame.Metalness.y);

    const float depth = DepthTexture[idx];

    const float depthView = ScreenToViewDepth(depth, Frame.CameraData);

    if (depthView < FP_Z || depth >= SKY_Z)
    {
        OutputTexture[idx] = MainTexture[idx];
        ReflectanceTexture[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        SpecularHitDist[idx] = 0.0f;
        return;
    }

    const snorm float4 normalRoughness = NormalRoughnessTexture[idx];
    const unorm float roughness = max(Scale01(normalRoughness.w, Frame.Roughness.x, Frame.Roughness.y), MIN_ROUGHNESS);

    const float3 positionVS = ScreenToViewPosition(uv, depthView, Frame.NDCToView);
    const float3 positionCS = ViewToWorldPosition(positionVS, Frame.ViewInverse);
    const float3 positionWS = positionCS + Frame.Position.xyz;

    const snorm float3 normalWS = normalize(normalRoughness.xyz);

    float3 albedo = Color::GammaToLinear(AlbedoTexture[idx].rgb);

    float3 specular = DEFAULT_SPECULAR;

    uint seed = InitRandomSeed(DispatchRaysIndex().xy, DispatchRaysDimensions().xy, Frame.FrameCount);

    float3 viewWS = normalize(-positionCS);

    // GI computation - select method based on compile-time define
    #if defined(DDGI_ENABLED)
        // DDGI: Use probe-based diffuse + ray-traced specular hybrid
        #if defined(LAMBERT)
            OutputTexture[idx] = float4(DDGILambertianIndirect(positionWS, normalWS, viewWS, albedo), 0.0f);
        #else
            float4 result = DDGIGGXIndirect(positionWS, geometryNormalWS, normalWS, viewWS, albedo, specular, roughness, metalness, 0, seed);

            OutputTexture[idx] = MainTexture[idx] + float4(result.rgb, 0.0f);

            float2 alpha = float2(roughness * roughness, roughness * roughness);

            float3 Ht = GGXSample(seed, alpha);
            float3 H = TangentToWorld(normalWS, Ht);
            float VdotH = max(dot(viewWS, H), EPSILON_DOT_CLAMP);

            ReflectanceTexture[idx] = float4(saturate(F_Schlick(specular, VdotH)), 0.0f);

            SpecularHitDist[idx] = result.a;
        #endif
    #else
        // Path tracing: Traditional ray-traced GI
        #if defined(LAMBERT)
            OutputTexture[idx] = float4(LambertianIndirect(positionWS, normalWS, albedo, 0, seed), 0.0f);
        #else
            float4 result = GGXIndirect(positionWS, geometryNormalWS, normalWS, viewWS, albedo, specular, roughness, metalness, 0, seed);

            OutputTexture[idx] = MainTexture[idx] + float4(result.rgb, 0.0f);

            float2 alpha = float2(roughness * roughness, roughness * roughness);

            float3 Ht = GGXSample(seed, alpha);
            float3 H = TangentToWorld(normalWS, Ht);
            float VdotH = max(dot(viewWS, H), EPSILON_DOT_CLAMP);

            ReflectanceTexture[idx] = float4(saturate(F_Schlick(specular, VdotH)), 0.0f);

            SpecularHitDist[idx] = result.a;
        #endif
    #endif
}
