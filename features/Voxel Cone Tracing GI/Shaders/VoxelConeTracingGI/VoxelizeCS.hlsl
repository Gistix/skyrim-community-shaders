#define COMPUTESHADER

#include "Common/SharedData.hlsli"
#include "Common/GBuffer.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"
#include "VoxelConeTracingGI/Octree.hlsli"
#include "VoxelConeTracingGI/Voxel.hlsli"

cbuffer VoxelizeCB : register(b0)
{
	float4 NDCToView;
	float2 RcpFrameDim;
	float Cell2Coord;
	float Resolution;
	float3 Center;
	uint MaxDepth;
	float3 Size;
	uint _pad0;
}

Texture2D<unorm float3> Albedo : register(t0);
Texture2D<unorm float> Depth : register(t1);
Texture2D<unorm float3> Normal : register(t2);

RWStructuredBuffer<Node> Octree : register(u0);
RWByteAddressBuffer NodeCount : register(u1);

RWStructuredBuffer<Voxel> Voxels : register(u2);
RWByteAddressBuffer VoxelCount : register(u3);

static const uint childrenConst[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

[numthreads(8, 8, 1)]
void main(int2 pixCoord : SV_DispatchThreadID)
{
	const float2 uv = (pixCoord + .5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 positionSS = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	if (any(positionSS < 0) || any(positionSS > 1))
		return;

	const float depth = Depth[pixCoord];
	const float depthLinear = SharedData::GetScreenDepth(depth);

    if (depthLinear < FP_Z || depth == 1.0)
		return;

	float3 positionVS = ScreenToViewPosition(positionSS, depthLinear, NDCToView);
	float3 positionWS = ViewToWorldPosition(positionVS, FrameBuffer::CameraViewInverse[eyeIndex]) + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	float3 octreeMin = Center - Size * 0.5f;
	float3 octreeMax = Center + Size * 0.5f;

	if (any(positionWS < octreeMin) || any(positionWS > octreeMax))
		return;

	const float3 albedo = Albedo[pixCoord];

	const half3 normalVS = GBuffer::DecodeNormal(Normal[pixCoord].xy);
	const float3 normalWS = normalize(ViewToWorldVector(normalVS, FrameBuffer::CameraViewInverse[eyeIndex]));

	uint nodeIdx = 0;

    [loop] 
	for (uint d = 0; d < MaxDepth; d++)
	{
		Node node = Octree[nodeIdx];

		uint nodeDepth = node.GetDepth();
		float nodeSize = node.Size(nodeDepth);
		float3 nodeSizeWS = nodeSize * Size;

        float3 nodeMinWS = octreeMin + node.min * Cell2Coord * Size;
		float3 nodeMaxWS = nodeMinWS + nodeSizeWS;

		uint childOctantIdx = GetOctantIdx(positionWS, nodeMinWS, nodeMaxWS);

		uint childIdx;
		InterlockedCompareExchange(Octree[nodeIdx].children[childOctantIdx], 0, ~0u, childIdx);

		if (childIdx == 0) {
			uint originalValue;
			
			uint idx;
			NodeCount.InterlockedAdd(0, 1, idx);

            float3 useCorner = float3(
				(childOctantIdx >> 0) & 1,
				(childOctantIdx >> 1) & 1,
				(childOctantIdx >> 2) & 1
			);   

			float3 childSize = nodeSizeWS * 0.5f;
			float3 childMinWS = nodeMinWS + useCorner * childSize;

			uint3 childMin = floor(((childMinWS - octreeMin) / Size) * Resolution);

			uint nd = d + 1;
			bool isLeaf = (nd == MaxDepth);
			bool hasVoxel = isLeaf;

            Node childNode;
			childNode.min = childMin;
			childNode.PackData(isLeaf, hasVoxel, nd);
			childNode.children = childrenConst;

            if (isLeaf) {
				uint voxelIdx;
				VoxelCount.InterlockedAdd(0, 1, voxelIdx);

				Voxel voxel;
				voxel.position = positionWS;
				voxel.normal = normalWS;
				voxel.albedo = albedo;
				voxel.emission = float3(depth, depthLinear, 0);

				Voxels[voxelIdx] = voxel;
				childNode.voxel = voxelIdx;
			} else {
				childNode.voxel = 0;
			}

			Octree[idx] = childNode;

			uint oldIdx;
			InterlockedExchange(Octree[nodeIdx].children[childOctantIdx], idx, oldIdx);

			nodeIdx = idx;
		} 
		else if (childIdx == ~0u) 
		{
			continue;
		} 
		else 
		{
			nodeIdx = childIdx;
		}  
	}
}