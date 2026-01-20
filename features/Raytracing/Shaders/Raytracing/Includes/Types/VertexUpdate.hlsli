#ifndef VERTEX_UPDATE_HLSL
#define VERTEX_UPDATE_HLSL

struct VertexUpdateData
{
	uint index;
	uint flags;
	uint vertexCount;
    uint boneOffset;
	float3 bonePivot;
#ifndef __cplusplus
	row_major
#endif	
	float4x4 skinRoot;
	uint pad;
};

#ifdef __cplusplus
static_assert(sizeof(VertexUpdateData) % 4 == 0);
#endif

#endif