#define COMPUTESHADER

#include "Common/Game.hlsli"
#include "Common/Color.hlsli"

#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"

#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

#include "VoxelConeTracingGI/VoxelConeTracingGI.hlsli"

#ifndef DIFFUSE_CONES
	#define DIFFUSE_CONES (5)
#endif

cbuffer VCTGICB : register(b0)
{
	VoxelConeTracingGI::ConstantBuffer VCTGI;
}

cbuffer ScreenTraceCB : register(b1)
{
	float4 NDCToView;
	float2 RcpFrameDim;
	float MaxMipLevel;
	float Pad0;
	float4 Cones[32];
}

struct StepSettings
{
	float Length;
	float Radius;
	float MipScale;
	float Alpha;
};

struct ConeSettings
{
	StepSettings Step;
	int Count;
	int MinAngle;	
	int Angle;
	int Distance;
	float Alpha;
	float Offset;
	float Strength;
	uint Pad0;	
	uint Pad1;	
};

cbuffer TraceSettingsCB : register(b2)
{
	ConeSettings Diffuse;
	ConeSettings Specular;
	uint ResolutionMode;
	uint AdditionalBounces;
	uint Pad1;
	uint Pad2;
	uint Pad3;	
};

Texture2D<unorm float> Depth			: register(t0);
Texture2D<unorm float3> NormalRoughness : register(t1);
Texture2D<float4> Reflectance			: register(t2);

#if VOXELMODE == MODE_ISOTROPIC
typedef half4 precision4;
#else
typedef float4 precision4;
#endif

#if VOXELMODE == MODE_ISOTROPIC
Texture3D<precision4> ClipVoxels[8]		: register(t3);
#elif VOXELMODE == MODE_SH1
Texture3D<precision4> ClipVoxelsSHR[8]	: register(t3);
Texture3D<precision4> ClipVoxelsSHG[8]	: register(t11);
Texture3D<precision4> ClipVoxelsSHB[8]	: register(t19);
#elif VOXELMODE == MODE_SH2
Texture3D<precision4> ClipVoxelsSH0[8]	: register(t3);
Texture3D<precision4> ClipVoxelsSH1[8]	: register(t11);
Texture3D<precision4> ClipVoxelsSH2[8]	: register(t19);
Texture3D<precision4> ClipVoxelsSH3[8]	: register(t27);
Texture3D<precision4> ClipVoxelsSH4[8]	: register(t35);
Texture3D<precision4> ClipVoxelsSH5[8]	: register(t43);
Texture3D<precision4> ClipVoxelsSH6[8]	: register(t51); 
#endif

SamplerState samplerVoxels				: register(s0);

RWTexture2D<half4> DiffuseGI			: register(u0);
RWTexture2D<half4> SpecularGI			: register(u1);

uint CalculateClipmap(float3 position)
{
	for (uint i=0; i < VCTGI.ClipmapCount; i++) {
		const VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[i];
		
		if (all(position >= clipmap.Min) && any(position <= clipmap.Max))
			return i;
	}
	
	return 0;
}

VoxelConeTracingGI::Clipmap GetClipmap(float3 position) 
{
	return VCTGI.Clipmaps[CalculateClipmap(position)];
}

VoxelConeTracingGI::Clipmap GetClipmap(float3 position, out uint clipmapIdx) 
{
	clipmapIdx = CalculateClipmap(position);
	return VCTGI.Clipmaps[clipmapIdx];
}

// Maybe make this a variable? I don't want to handle padding again...
float GetVoxelSize(VoxelConeTracingGI::Clipmap clipmap)
{
	return (clipmap.Size * VCTGI.ResRcp) * RCP_SCALE;
}

/*half4 SampleClipmapVolume(uint clipmapIdx, Texture3D<precision4> volume[8], SamplerState samplerState, float3 location, float lod) {
	if (clipmapIdx == 0)
		return volume[0].SampleLevel(samplerState, location, lod);
	else if (clipmapIdx == 1)
		return volume[1].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 2)
		return volume[2].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 3)
		return volume[3].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 4)
		return volume[4].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 5)
		return volume[5].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 6)
		return volume[6].SampleLevel(samplerState, location, lod);		
	else
		return volume[7].SampleLevel(samplerState, location, lod);			
}*/

precision4 SampleClipmapVolume(uint clipmapIdx, Texture3D<precision4> volume[8], SamplerState samplerState, float3 location, float lod) {
	if (clipmapIdx == 0)
		return volume[0].SampleLevel(samplerState, location, lod);
	else if (clipmapIdx == 1)
		return volume[1].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 2)
		return volume[2].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 3)
		return volume[3].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 4)
		return volume[4].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 5)
		return volume[5].SampleLevel(samplerState, location, lod);	
	else if (clipmapIdx == 6)
		return volume[6].SampleLevel(samplerState, location, lod);		
	else
		return volume[7].SampleLevel(samplerState, location, lod);			
}

precision4 SampleVoxels(uint clipmapIdx, SamplerState samplerState, float3 location, float lod, float3 direction) 
{
#if VOXELMODE == MODE_ISOTROPIC
	return SampleClipmapVolume(clipmapIdx, ClipVoxels, samplerState, location, lod);
#elif VOXELMODE == MODE_SH1
	sh2 shR = SampleClipmapVolume(clipmapIdx, ClipVoxelsSHR, samplerState, location, lod);
	sh2 shG = SampleClipmapVolume(clipmapIdx, ClipVoxelsSHG, samplerState, location, lod);
	sh2 shB = SampleClipmapVolume(clipmapIdx, ClipVoxelsSHB, samplerState, location, lod);
	
	float3 rgbColor = max(0.0f, SphericalHarmonics::Unproject(shR, shG, shB, direction));
	
	return precision4(rgbColor, 1.0f); // We need alpha for visibility, it needs to be mipmapped as well, so maybe a one channel 3D texture
#endif
}

float4 ConeTraceOctree(float3 origin, float3 normal, float3 direction, float aperture, ConeSettings cone)
{
	VoxelConeTracingGI::Clipmap clipmap = GetClipmap(origin);
	
	float voxelSize = GetVoxelSize(clipmap);
	
    float3 color = float3(0, 0, 0);
    float alpha = 0;
    float dist = voxelSize;
    float occlusion = 0;
        
    float3 startPos = origin + (normal * voxelSize * cone.Offset);

	float maxDistance = cone.Distance * (M_TO_GAME_UNIT);
	
    while(dist < maxDistance && alpha < cone.Alpha) {	
        float diameter = max(voxelSize, aperture * dist * cone.Step.Radius);
          
        float3 samplePos = startPos + dist * direction;
		
		uint clipIdx;
		clipmap = GetClipmap(samplePos, clipIdx);
		voxelSize = GetVoxelSize(clipmap);
		
        float mipLevel = clamp(log2(diameter / voxelSize), 0.0, MaxMipLevel * cone.Step.MipScale);     
            
        float3 volumeCoord = ((samplePos - clipmap.Min) * clipmap.SizeRcp) * RCP_SCALE;
            
		precision4 sampledColor = SampleVoxels(clipIdx, samplerVoxels, volumeCoord, mipLevel, direction);

        color += (1.0f - saturate(alpha * cone.Step.Alpha)) * sampledColor.rgb;
        occlusion += ((1.0f - alpha) * sampledColor.a) / (1.0f + 0.03f * diameter);
		alpha += (1.0f - alpha) * sampledColor.a;
		dist += diameter * cone.Step.Length;
    }
        
    return float4(color * cone.Strength, occlusion);
}  

void OrthonormalBasis(float3 N, out float3 tangent, out float3 bitangent)
{
    float sign = (N.z >= 0) ? 1.0 : -1.0;
    float a = -1.0 / (sign + N.z);
    float b = N.x * N.y * a;

    tangent = float3(1.0 + sign * N.x * N.x * a, sign * b, -sign * N.x);
    bitangent = float3(b, sign + N.y * N.y * a, -N.y);
}  

[numthreads(16, 8, 1)]
void main(int2 id: SV_DispatchThreadID)
{
	const float2 uv = (id + .5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 positionSS = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	if (any(positionSS < 0) || any(positionSS > 1))
		return;

	const unorm float depth = Depth[id];
	const unorm float depthLinear = SharedData::GetScreenDepth(depth);

	const unorm float3 normalRoughness = NormalRoughness[id];
	const unorm float roughness = normalRoughness.z;	
	const unorm float4 reflectance = Reflectance[id];	
	
	const float3 positionVS = ScreenToViewPosition(positionSS, depthLinear, NDCToView);
	//float3 positionWS = FrameBuffer::ViewToWorld(positionVS, true, eyeIndex) + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	const float3 positionCS = ViewToWorldPosition(positionVS, FrameBuffer::CameraViewInverse[eyeIndex]);
	const float3 positionWS = positionCS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	
	const half3 normalVS = GBuffer::DecodeNormal(normalRoughness.xy);
	//const float3 normalWS = normalize(FrameBuffer::ViewToWorld(normalVS, false, eyeIndex));
	const float3 normalWS = normalize(ViewToWorldVector(normalVS, FrameBuffer::CameraViewInverse[eyeIndex]));	
	
	const VoxelConeTracingGI::Clipmap lastClipmap = VCTGI.Clipmaps[VCTGI.ClipmapCount-1];
	
	[branch]
	if (any(positionWS < lastClipmap.Min) || any(positionWS > lastClipmap.Max)) {
		DiffuseGI[id] = half4(0, 0, 0, 0);
		SpecularGI[id] = half4(0, 0, 0, 0);
		return;
	}
	
    float3 tangent, bitangent;
    OrthonormalBasis(normalWS, tangent, bitangent); 	
	
	// Diffuse
	{      	
		float4 indirectDiffuse = float4(0, 0, 0, 0);
		float sum = 0.0f;
		
		const float diffuseAperture = tan(radians(Diffuse.Angle));

		[unroll]
		for (int i=0; i < DIFFUSE_CONES; i++) {
			const float3 coneDir = Cones[i].xyz;
			const float3 coneDirWS = normalize(coneDir.x * tangent + coneDir.y * bitangent + coneDir.z * normalWS);
			
			const float cosTheta = saturate(dot(normalWS, coneDirWS));
			
			indirectDiffuse += ConeTraceOctree(positionWS, normalWS, coneDirWS, diffuseAperture, Diffuse) * cosTheta;
			sum += cosTheta;
		}
		
		DiffuseGI[id] = indirectDiffuse / sum;		
	}

	// Specular
	{          
		const float3 invViewWS = normalize(positionCS);
		//const float3 viewWS = normalize(FrameBuffer::CameraPosAdjust[eyeIndex].xyz - positionWS);
		
		const float3 reflectWS = reflect(invViewWS, normalWS);

		const float specularAperture = tan(radians(lerp(Specular.MinAngle, Specular.Angle, roughness)));

		SpecularGI[id] = ConeTraceOctree(positionWS, normalWS, reflectWS, specularAperture, Specular) * half4(0.05.rrr + reflectance.rgb, 1.0f);
	}
		
}