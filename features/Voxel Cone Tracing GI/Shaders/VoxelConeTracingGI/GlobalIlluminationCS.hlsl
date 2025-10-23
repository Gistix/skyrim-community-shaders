#define COMPUTESHADER

#include "Common/Game.hlsli"
#include "Common/Color.hlsli"

#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"

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
Texture2D<float4> Reflectance		: register(t2);
Texture3D<half4> Voxels					: register(t3);

SamplerState samplerVoxels				: register(s3);

RWTexture2D<half4> DiffuseGI			: register(u0);
RWTexture2D<half4> SpecularGI			: register(u1);

half4 ConeTraceOctree(float3 origin, float3 normal, float3 direction, float aperture, ConeSettings cone)
{
	uint clipIdx = 0;
	VoxelConeTracingGI::Clipmap clipmap = VCTGI.Clipmaps[clipIdx]; // hardcoded for now
	
	float voxelSize = (clipmap.Size * VCTGI.ResRcp) * RCP_SCALE;
	
    float3 color = float3(0, 0, 0);
    float alpha = 0;
    float dist = voxelSize;
    float occlusion = 0;
        
    float3 startPos = origin + (normal * voxelSize * cone.Offset);

	float maxDistance = cone.Distance * (M_TO_GAME_UNIT);
	
    while(dist < maxDistance && alpha < cone.Alpha) {	
        float diameter = max(voxelSize, aperture * dist * cone.Step.Radius);
            
        float3 samplePos = startPos + dist * direction;
        float mipLevel = clamp(log2(diameter / voxelSize), 0.0, MaxMipLevel * cone.Step.MipScale);     
            
        float3 volumeCoord = ((samplePos - clipmap.Min) * clipmap.SizeRcp) * RCP_SCALE;
            
		half4 sampledColor = Voxels.SampleLevel(samplerVoxels, volumeCoord, mipLevel);

        color += (1.0f - saturate(alpha * cone.Step.Alpha)) * sampledColor.rgb;
        occlusion += ((1.0f - alpha) * sampledColor.a) / (1.0f + 0.03f * diameter);
		alpha += (1.0f - alpha) * sampledColor.a;
		dist += diameter * cone.Step.Length;
    }
        
    return half4(color * cone.Strength, occlusion);
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