#ifndef __VOXELCONETRACINGGI_NAMESPACE_DEPENDENCY_HLSL__
#define __VOXELCONETRACINGGI_NAMESPACE_DEPENDENCY_HLSL__

namespace VoxelConeTracingGI
{
    struct Clipmap 
	{
		float3 Min;
		float Size;
		float3 Max;
		float SizeRcp;
	};
	
	struct ConstantBuffer
	{
		uint Res;
		float ResRcp; 
		uint ClipmapCount;
		uint Pad0;
		Clipmap Clipmaps[8];		
	};
}

#endif