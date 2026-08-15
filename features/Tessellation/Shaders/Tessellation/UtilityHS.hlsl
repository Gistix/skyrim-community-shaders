#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/UtilityCommon.hlsli"

PatchConstantOutput PatchConstant(InputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch)
{
	return CalculateTessFactors(
		a_patch[0].PositionCS, a_patch[1].PositionCS, a_patch[2].PositionCS,
		a_patch[0].WorldPosition.xyz, a_patch[1].WorldPosition.xyz, a_patch[2].WorldPosition.xyz);
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(INPUT_PATCH_SIZE)]
[patchconstantfunc("PatchConstant")]
UtilityInOut main(InputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
