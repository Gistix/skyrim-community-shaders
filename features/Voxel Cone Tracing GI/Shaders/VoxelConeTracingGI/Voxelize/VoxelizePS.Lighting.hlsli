cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
	float4 ShadowLightMaskSelect : packoffset(c2);
	float4 MaterialData : packoffset(c3);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c4);
	float3 EmitColor : packoffset(c4.y);
	float4 ProjectedUVParams : packoffset(c6);
	float4 SSRParams : packoffset(c7);
	float4 WorldMapOverlayParametersPS : packoffset(c8);
	float4 ProjectedUVParams2 : packoffset(c9);
	float4 ProjectedUVParams3 : packoffset(c10);  // fProjectedUVDiffuseNormalTilingScale in x, fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c11);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c14);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[7] : packoffset(c15);               // point light radius in w
	float4 PointLightColor[7] : packoffset(c22);
	float2 NumLightNumShadowLight : packoffset(c29);
#	else
	// VR is [49] instead of [30]
	float3 DirLightDirection : packoffset(c0);
	float4 UnknownPerGeometry[12] : packoffset(c1);
	float3 DirLightColor : packoffset(c13);
	float4 ShadowLightMaskSelect : packoffset(c14);
	float4 MaterialData : packoffset(c15);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c16);
	float3 EmitColor : packoffset(c16.y);
	float4 ProjectedUVParams : packoffset(c18);
	float4 SSRParams : packoffset(c19);
	float4 WorldMapOverlayParametersPS : packoffset(c20);
	float4 ProjectedUVParams2 : packoffset(c21);
	float4 ProjectedUVParams3 : packoffset(c22);  // fProjectedUVDiffuseNormalTilingScale in x,	fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c23);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c26);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[14] : packoffset(c27);              // point light radius in w
	float4 PointLightColor[7] : packoffset(c41);
	float2 NumLightNumShadowLight : packoffset(c48);
#	endif  // VR
};

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexGlowSampler : register(t6);

SamplerState SampColorSampler : register(s0);
SamplerState SampGlowSampler : register(s6);

void FillVoxelSample(inout Voxel voxelSample, in PS_INPUT input) {
	float2 uv = input.TexCoord0.xy;
	
	/*float3 directionalAmbientColor = max(0, mul(DirectionalAmbient, float4(ambientNormal, 1.0)));

	#if defined(IBL)
		if (SharedData::iblSettings.EnableDiffuseIBL) {
			if (SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection) {
				directionalAmbientColor = ImageBasedLighting::GetStaticDiffuseIBL(ambientNormal, SampColorSampler);
			} else if (!SharedData::InInterior || SharedData::iblSettings.EnableInterior) {
				directionalAmbientColor *= SharedData::iblSettings.DALCAmount;
			}
		}
	#endif*/
	
	voxelSample.Albedo = Color::GammaToLinear(Color::Diffuse(TexColorSampler.Sample(SampColorSampler, uv).rgb));
	voxelSample.Emissive = Color::GammaToLinear(EmitColor * Color::Diffuse(TexGlowSampler.Sample(SampGlowSampler, uv).rgb));
}