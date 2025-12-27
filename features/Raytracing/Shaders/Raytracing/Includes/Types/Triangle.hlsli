#ifndef TRIANGLE_HLSL
#define TRIANGLE_HLSL

#include "Raytracing/Includes/Types/Vertex.hlsli"

struct Data
{
	uint R : 8;
	uint G : 8;
	uint B : 8;
	uint A : 7;	
	uint Handedness : 1;
};
#ifdef __cplusplus
static_assert(sizeof(Data) == 4);
#endif

struct VertexRT
{
	half2 Texcoord0;
	half3 Normal;
	half3 Tangent;
	Data Data;
	
#ifndef __cplusplus
    half4 Color()
    {
		return half4(half(Data.R) / 255.0h, half(Data.G) / 255.0h, half(Data.B) / 255.0h, half(Data.A) / 127.0h);
    }
	
    half Handedness()
    {
		return Data.Handedness ? 1.0h : -1.0h;
    }
#endif
};
#ifdef __cplusplus
static_assert(sizeof(VertexRT) % 4 == 0);
#endif

struct Triangle
{
	VertexRT v0;
	VertexRT v1;
	VertexRT v2;
};
#ifdef __cplusplus
static_assert(sizeof(Triangle) % 4 == 0);
#endif

#endif