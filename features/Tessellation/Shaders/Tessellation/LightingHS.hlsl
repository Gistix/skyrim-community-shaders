#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/LightingCommon.hlsli"

PatchConstantOutput PatchConstant(InputPatch<LightingInOut, INPUT_PATCH_SIZE> a_patch)
{
	return CalculateTessFactors(
		a_patch[0].Position, a_patch[1].Position, a_patch[2].Position,
		a_patch[0].WorldPosition.xyz, a_patch[1].WorldPosition.xyz, a_patch[2].WorldPosition.xyz);
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(INPUT_PATCH_SIZE)]
[patchconstantfunc("PatchConstant")]
LightingInOut main(InputPatch<LightingInOut, INPUT_PATCH_SIZE> a_patch, uint a_controlPointID : SV_OutputControlPointID)
{
	return a_patch[a_controlPointID];
}
