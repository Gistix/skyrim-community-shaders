#include "Common/FrameBuffer.hlsli"
#include "Tessellation/Common.hlsli"
#include "Tessellation/UtilityCommon.hlsli"

PatchConstantOutput PatchConstant(InputPatch<UtilityInOut, INPUT_PATCH_SIZE> a_patch)
{
	PatchConstantOutput output;
	float factor = max(Factor, 1.0);
	output.tessFactor[0] = factor;
	output.tessFactor[1] = factor;
	output.tessFactor[2] = factor;
	output.insideTessFactor = factor;
	return output;
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
