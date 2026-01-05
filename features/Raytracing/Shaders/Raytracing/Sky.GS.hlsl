struct GS_INOUT
{
	float4 Position : SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0 : TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0 : TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0 : TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1 : TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2 : TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color : COLOR0;
#endif

	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
	uint EyeIndex : EYEIDX0;
#endif  // VR
};

[maxvertexcount(3)]
void main(triangle GS_INOUT input[3], inout TriangleStream<GS_INOUT> outputStream)
{
	GS_INOUT output[3];
	
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        float3 dir = normalize(input[i].WorldPosition.xyz);

		dir.z = max(dir.z, 0.0f);
		
        // 1. Full sphere / hemisphere projection
        float r = sqrt(1.0f - dir.z);

        float phi = atan2(dir.y, dir.x);

        float2 disk = r * float2(cos(phi), sin(phi));

        // 2. Map to clip space [-1,1]
        output[i].Position = float4(disk, dir.z * 0.999f, 1.0f); // input[i].WorldPosition.z / 300.0f // The top vertex of the atmosphere dome has a z position of 270
		
#if defined(DITHER) || defined(TEX) || defined(HORIZFADE)
		output[i].TexCoord0 = input[i].TexCoord0;
#endif

#if defined(TEXLERP)
		output[i].TexCoord1 = input[i].TexCoord1;
#endif

#if defined(HORIZFADE)
		output[i].TexCoord2 = input[i].TexCoord2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
		output[i].Color = input[i].Color;
#endif

		output[i].WorldPosition = input[i].WorldPosition;
		output[i].PreviousWorldPosition = input[i].PreviousWorldPosition;	
		
#if defined(VR)
		output[i].ClipDistance = input[i].ClipDistance;
		output[i].CullDistance = input[i].CullDistance;
		output[i].EyeIndex = input[i].EyeIndex;
#endif  // VR		
		
		outputStream.Append(output[i]);
    }
	
	outputStream.RestartStrip();
}