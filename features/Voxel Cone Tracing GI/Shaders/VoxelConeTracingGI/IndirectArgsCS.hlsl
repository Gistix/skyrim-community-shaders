RWByteAddressBuffer VoxelCount				: register(u0);
RWByteAddressBuffer VoxelIndirectArgs		: register(u1);
RWByteAddressBuffer VoxelDrawIndirectArgs	: register(u2);

[numthreads(1, 1, 1)]
void main(int id : SV_DispatchThreadID)
{
	uint voxelCount = VoxelCount.Load(4 * id);
	
	uint threadGroups = ceil(voxelCount / 64.0);
	
	VoxelIndirectArgs.Store(4 * (id * 3), threadGroups);	
	VoxelDrawIndirectArgs.Store(4 * ((id * 4) + 1), voxelCount);
}