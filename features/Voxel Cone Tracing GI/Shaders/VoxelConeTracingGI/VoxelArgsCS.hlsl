ByteAddressBuffer VoxelCount : register(t0);
RWByteAddressBuffer VoxelArgs : register(u0);

[numthreads(1, 1, 1)]
void main(int id : SV_DispatchThreadID)
{
	uint voxelCount = VoxelCount.Load(0);

	uint threadGroups = ceil(voxelCount / 64.0);

	VoxelArgs.Store(0, threadGroups);
	VoxelArgs.Store(4, 1);
	VoxelArgs.Store(8, 1);
}