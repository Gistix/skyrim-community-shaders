/*
* This file defines a new feature template for Community Shader.
* Copy the .h and .cpp files to src/Features and rename them to your feature's name.
* Replace all VoxelConeTracingGI occurances in both files as well, and change the metadata accordingly.
* Don't forget to add the feature singleton to src/Feature.cpp, Globals.h & Globals.cpp
* and copy and rename the "New Feature" folder and contents to features/ so it gets registered.
*
* The naming and coding style are adapted to my personal practice,
* but we don't really have a strict, solidified guideline on that.
* So take your liberties within reason.
*
* Cheers,
* ProfJack
* 2025-06-28
*/

#pragma once

struct VoxelConeTracingGI : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "Voxel Cone Tracing GI"; }
	virtual inline std::string GetShortName() override { return "VoxelConeTracingGI"; }
	virtual inline std::string_view GetCategory() const override { return "Lighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"This is a terse description.",
			{
				"This is a subfeature.",
				"This is another subfeature.",
				"Cheese.",
			}
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "SHADER_MACRO"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Lighting; };

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileShaders();
	void CompileComputeShaders();

	// Do stuff
	void Voxelize();
	void InjectLighting();
	void Volumize();

	virtual void Prepass() override;

	const char* dimensionsLabels[8] = { "4", "8", "16", "32", "64", "128", "256", "512" };
	const uint dimensions[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		int lod0Resolution = 4;
		int lod1Resolution = 4;
		int lod2Resolution = 4;
		int lod3Resolution = 4;

		int lod0Size = 20;
		int lod1Size = 60;
		int lod2Size = 150;
		int lod3Size = 400;

		int maxLights = 64;
	} settings;


	uint NodeCount(uint maxDepth)
	{
		return static_cast<uint>(round((pow(8, maxDepth + 1) - 1) / 7.0));
	}

	uint LeafNodeCount(uint maxDepth)
	{
		return static_cast<uint>(pow(8, maxDepth));
	}

	uint OctreeMaxDepth(uint lod=0) {
		uint lodResolution = GetLodResolution(lod);
		return static_cast<uint>(ceil(log2(lodResolution)));
	}

	uint GetLodResolution(uint lod=0)
	{
		switch (lod) {
			case 1:
				return dimensions[settings.lod1Resolution];
			case 2:
				return dimensions[settings.lod2Resolution];
			case 3:
				return dimensions[settings.lod3Resolution];
			default:
				return dimensions[settings.lod0Resolution];
		}
	}

	uint MipLevels(uint resolution) 
	{
		return static_cast<uint>(log2(resolution));
	}

	static inline bool IsValidLight(RE::BSLight* a_light);
	static inline bool IsGlobalLight(RE::BSLight* a_light);

	struct Node
	{
		float3 min;
		float3 max;
		uint voxel;
		uint children[8];
	};
	static_assert(sizeof(Node) % 4 == 0);

	struct Voxel
	{
		float3 position;
		// no support for half3? :(
		float3 albedo;
		float3 normal;
		float3 emission;
	};
	static_assert(sizeof(Voxel) % 4 == 0);

	struct Light
	{
		float3 position;
		float range;		
		float3 color;
		uint type;
	};
	static_assert(sizeof(Light) % 16 == 0);

	eastl::vector<Light> lights = {};

	struct CbData
	{
		float3 ColorA;
		float _pad0;  // Padding to align to 16 bytes
		DirectX::XMUINT2 IdA;
		float2 UvA;
	};
	static_assert(sizeof(CbData) % 16 == 0,
		"CbData must be aligned to 16 bytes. "
		"Check out maraneshi.github.io/HLSL-ConstantBufferLayoutVisualizer/ if you're unsure.");

	//eastl::unique_ptr<ConstantBuffer> cheeseCb = nullptr;  // Omit this if you want to put your CB in src/FeatureBuffer.cpp
	winrt::com_ptr<ID3D11ComputeShader> cheeseCs = nullptr;

	eastl::unique_ptr<StructuredBuffer> nodeBuffer = nullptr;
	eastl::unique_ptr<StructuredBuffer> voxelBuffer = nullptr;
	eastl::unique_ptr<ConstantBuffer> lightBuffer = nullptr;

	eastl::unique_ptr<Texture3D> lod0Tex = nullptr;
	winrt::com_ptr<ID3D11SamplerState> lod0Sampler = nullptr;
};