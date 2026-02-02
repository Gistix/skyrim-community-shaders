#ifndef INSTANCE_HLSL
#define INSTANCE_HLSL

#include "Raytracing/Includes/Types/LightData.hlsli"

#ifdef __cplusplus
struct InstanceData
#else
struct Instance
#endif
{
#ifndef __cplusplus
    row_major
#endif
	float3x4 Transform;
    LightData LightData;
};

#ifdef __cplusplus
static_assert(sizeof(InstanceData) % 4 == 0);
#endif

#endif