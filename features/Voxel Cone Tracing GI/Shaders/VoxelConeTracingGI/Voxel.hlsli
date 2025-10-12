#ifndef __VOXELCONETRACINGGI_VOXEL_DEPENDENCY_HLSL__
#define __VOXELCONETRACINGGI_VOXEL_DEPENDENCY_HLSL__
    struct Voxel
    {
        uint3 Coord;
        float3 Albedo;
        float3 Emission;
        float3 Normal;
    };
#endif // __VOXELCONETRACINGGI_VOXEL_DEPENDENCY_HLSL__
