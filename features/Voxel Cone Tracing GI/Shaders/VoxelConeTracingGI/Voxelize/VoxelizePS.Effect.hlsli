cbuffer PerMaterial : register(b1)
{
	float4 BaseColor : packoffset(c0);
	float4 BaseColorScale : packoffset(c1);
	float4 LightingInfluence : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float4 PLightPositionX[1] : packoffset(c0);
	float4 PLightPositionY[1] : packoffset(c1);
	float4 PLightPositionZ[1] : packoffset(c2);
	float4 PLightingRadiusInverseSquared : packoffset(c3);
	float4 PLightColorR : packoffset(c4);
	float4 PLightColorG : packoffset(c5);
	float4 PLightColorB : packoffset(c6);
	float4 DLightColor : packoffset(c7);
	float4 PropertyColor : packoffset(c8);
	float4 AlphaTestRef : packoffset(c9);
	float4 MembraneRimColor : packoffset(c10);
	float4 MembraneVars : packoffset(c11);
#	else
	float4 PLightPositionX[2] : packoffset(c0);
	float4 PLightPositionY[2] : packoffset(c2);
	float4 PLightPositionZ[2] : packoffset(c4);
	float4 PLightingRadiusInverseSquared : packoffset(c6);
	float4 PLightColorR : packoffset(c7);
	float4 PLightColorG : packoffset(c8);
	float4 PLightColorB : packoffset(c9);
	float4 DLightColor : packoffset(c10);
	float4 PropertyColor : packoffset(c11);  // VR should be 11; this could start earlier though
	float4 AlphaTestRef : packoffset(c12);
	float4 MembraneRimColor : packoffset(c13);
	float4 MembraneVars : packoffset(c14);
#	endif
};

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexNoiseSampler : register(t2);
Texture2D<float4> TexGrayscaleSampler : register(t4);

SamplerState SampBaseSampler : register(s0);
SamplerState SampNoiseSampler : register(s2);
SamplerState SampGrayscaleSampler : register(s4);

void FillVoxelSample(inout Voxel voxelSample, in PS_INPUT input) {
	float2 uv = input.TexCoord0.xy;
	
	float4 baseTexColor = float4(1, 1, 1, 1);
	float4 baseColor = float4(1, 1, 1, 1);
	
#if !defined(TEXTURE)
	[branch] 
	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
#endif
	{
		baseTexColor = TexBaseSampler.Sample(SampBaseSampler, uv);
		baseColor *= baseTexColor;
		if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::IgnoreTexAlpha || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha) {
			baseColor.w = 1;
		}
	}
	
#if defined(MEMBRANE)
	float4 baseColorMul = float4(1, 1, 1, 1);
#else
	float4 baseColorMul = BaseColor;
	
	#if defined(VC) && !defined(PROJECTED_UV)
		baseColorMul *= input.Color;
	#endif
#endif

#if defined(MEMBRANE)
	//baseColor.w *= input.ViewVector.w;
#else
	baseColor.w *= input.TexCoord0.z;
#endif

	baseColor = baseColorMul * baseColor;	
	//baseColor.w *= softMul;
	
	float alpha = baseColor.w;	
	
	float baseColorScale = BaseColorScale.x;

	#if defined(MEMBRANE)
		float4 membraneColor = MembraneRimColor; // Just a hack
		
		baseColor.xyz = (PropertyColor.xyz + baseColor.xyz) * alpha + membraneColor.xyz * membraneColor.w;
		alpha += membraneColor.w;
		baseColorScale = MembraneVars.z;
	#endif

	[branch] 
	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor)
	{
		float2 grayscaleToColorUv = float2(baseTexColor.y, baseColorMul.x);
	#if defined(MEMBRANE)
		grayscaleToColorUv.y = PropertyColor.x;
	#endif
		baseColor.xyz = baseColorScale * TexGrayscaleSampler.Sample(SampGrayscaleSampler, grayscaleToColorUv).xyz;
	}	
	
	float3 linBaseColor = Color::GammaToLinear(baseColor.xyz); 
	
	voxelSample.Albedo = linBaseColor;
	voxelSample.Emissive = linBaseColor * LightingInfluence.x;
}