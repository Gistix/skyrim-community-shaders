#ifndef __VOXELCONETRACINGGI_OCTREE_DEPENDENCY_HLSL__
#define __VOXELCONETRACINGGI_OCTREE_DEPENDENCY_HLSL__

#define INVALID_CHILD_INDEX 0

#define IS_LEAF_BIT 0x0
#define HAS_VOXEL_BIT 0x1

#define DEPTH_SHIFT 0x2
#define DEPTH_MASK 0xFFu // 8 bits = 0b11111111 = 255

#define SIZE_SCALAR ~0u
#define SIZE_SCALAR_INV 1.0 / SIZE_SCALAR

struct Node
{
	uint3 min;
	uint voxel;
    uint data;
	uint children[8];
    
    uint GetDepth() {
        return (data >> DEPTH_SHIFT) & DEPTH_MASK;
    }   
    
    float Size(uint depth) {
        return 1.0 / (1 << depth);
    }    
       
    void PackData(bool isLeaf, bool hasVoxel, uint depth)
    {
        data = (uint(isLeaf) << IS_LEAF_BIT) | (uint(hasVoxel) << HAS_VOXEL_BIT) | ((depth & DEPTH_MASK) << DEPTH_SHIFT);
    }

    void UnpackData(out bool isLeaf, out bool hasVoxel, out uint depth)
    {
        isLeaf = ((data >> IS_LEAF_BIT) & 0x1u) != 0;
        hasVoxel = ((data >> HAS_VOXEL_BIT) & 0x1u) != 0;
        depth = (data >> DEPTH_SHIFT) & DEPTH_MASK;
    }    
};

uint GetOctantIdx(float3 pos, float3 min, float3 max)
{
    float3 center = 0.5 * (min + max);
    uint3 greater = (uint3)(pos > center);
    return dot(greater, uint3(1, 2, 4));
}

uint GetTraversalOrderIndex(float3 dir)
{
    return ((uint)(dir.x < 0) << 2) | ((uint)(dir.y < 0) << 1) | ((uint)(dir.z < 0) << 0);
}

static const uint ChildOrderTable[8][8] = {
    {0,1,2,3,4,5,6,7}, // +x +y +z
    {0,1,2,3,6,7,4,5}, // +x +y -z
    {0,1,4,5,2,3,6,7}, // +x -y +z
    {0,1,6,7,2,3,4,5}, // +x -y -z
    {4,5,6,7,0,1,2,3}, // -x +y +z
    {6,7,4,5,0,1,2,3}, // -x +y -z
    {2,3,6,7,0,1,4,5}, // -x -y +z
    {6,7,2,3,0,1,4,5}  // -x -y -z
};

bool IntersectAABB(float3 origin, float3 dir, float3 minB, float3 maxB, out float tmin, out float tmax)
{
    float3 invDir = 1.0 / dir;
    float3 t0 = (minB - origin) * invDir;
    float3 t1 = (maxB - origin) * invDir;

    float3 tmin3 = min(t0, t1);
    float3 tmax3 = max(t0, t1);

    tmin = max(max(tmin3.x, tmin3.y), tmin3.z);
    tmax = min(min(tmax3.x, tmax3.y), tmax3.z);

    return tmax >= max(tmin, 0.0);
}

#endif // __VOXELCONETRACINGGI_OCTREE_DEPENDENCY_HLSL__
