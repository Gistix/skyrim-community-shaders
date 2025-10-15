RWByteAddressBuffer VoxelIndirectArgs : register(u0);

[numthreads(1, 1, 1)]
void main(int id : SV_DispatchThreadID)
{
	uint voxelCount = VoxelIndirectArgs.Load(4 * 1);
	
	uint threadGroups = ceil(voxelCount / 64.0);
	
	VoxelIndirectArgs.Store(4 * 8, threadGroups);	
}