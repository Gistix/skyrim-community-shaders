#ifndef __VOXELCONETRACINGGI_LIGHT_DEPENDENCY_HLSL__
#define __VOXELCONETRACINGGI_LIGHT_DEPENDENCY_HLSL__

	struct Light
	{
		float3 lvector;
		float range;		
		float3 color;
		uint type;
	};
#endif // __VOXELCONETRACINGGI_LIGHT_DEPENDENCY_HLSL__
