#include "Upscaling/UpscaleVS.hlsl"

#ifdef PSHADER

Texture2D<float> DepthInput : register(t0);

struct PS_OUTPUT
{
	float Depth : SV_Depth;
};

PS_OUTPUT main(VS_OUTPUT input)
{
	PS_OUTPUT output;
	output.Depth = DepthInput.Load(int3(input.Position.xy, 0));
	return output;
}

#endif