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
	void VoxelArgs();
	void InjectLighting();
	void Debug();

	virtual void PostPostLoad() override;

	virtual void Prepass() override;

	const char* dimensionsLabels[8] = { "4", "8", "16", "32", "64", "128", "256", "512" };
	const uint dimensions[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };
	static constexpr uint MAX_LIGHTS = 1024;
	static constexpr uint MAX_MESHES = 2048;

	float3 center;

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		uint noIndoorDir = false;
		int lod0Resolution = 4;
		int lod1Resolution = 4;
		int lod2Resolution = 4;
		int lod3Resolution = 4;

		int lod0Size = 20;
		int lod1Size = 60;
		int lod2Size = 150;
		int lod3Size = 400;
	} settings;

	void BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass);

	struct Vertex
	{
		float4 position;
		uint16_t uv;
		uint16_t normal;
	};

	struct Mesh
	{
		eastl::vector<Vertex> vertices;
		eastl::vector<int> indices;
	};

	eastl::hash_map<const char*, Mesh> meshes;

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct ValidLight
		{
			static bool thunk(RE::BSShaderProperty* a_property, RE::BSLight* a_light)
			{
				return func(a_property, a_light) && (a_light->portalStrict || !a_light->portalGraph || a_light->IsShadowLight());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		using ValidLight1 = ValidLight<1>;
		using ValidLight2 = ValidLight<2>;
		using ValidLight3 = ValidLight<3>;

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);

			stl::write_thunk_call<ValidLight1>(REL::RelocationID(100994, 107781).address() + 0x92);
			stl::write_thunk_call<ValidLight2>(REL::RelocationID(100997, 107784).address() + REL::Relocate(0x139, 0x12A));
			stl::write_thunk_call<ValidLight3>(REL::RelocationID(101296, 108283).address() + REL::Relocate(0xB7, 0x7E));

			logger::info("[VCT] Installed hooks");
		}
	};

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
		return static_cast<uint>(log2(resolution)) + 1;
	}

	static inline bool IsValidLight(RE::BSLight* a_light);
	static inline bool IsGlobalLight(RE::BSLight* a_light);

	struct Node
	{
		uint min[3];
		uint voxel;
		uint data;
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
		float3 lvector;
		float range;		
		float3 color;
		uint type;
	};
	static_assert(sizeof(Light) % 4 == 0);

	struct alignas(16) VoxelizeCB
	{
		float3 Center;
		uint MaxDepth;
		float3 Size;
		float Cell2Coord;
		float3 _pad0;
		float Resolution;	
	} voxelizeCBData;
	static_assert(sizeof(VoxelizeCB) % 16 == 0);

	struct alignas(16) InjectLightingCB
	{
		float3 VoxelSize;
		uint LightCount;
		float3 Coord2Cell;
		uint _pad0;
		float3 Min;
		uint _pad1;	
	} injectLightingCBData;
	static_assert(sizeof(InjectLightingCB) % 16 == 0);

	struct alignas(16) DebugCB
	{
		float4 NDCToView;

		float2 RcpFrameDim;
		float2 _pad0;

		float3 Coord2Cell;
		uint _pad1;

		float3 Min;
		uint pad2;
	} debugCBData;
	static_assert(sizeof(DebugCB) % 16 == 0);

	eastl::vector<Light> lights = {};

	eastl::unique_ptr<ConstantBuffer> voxelizeCB = nullptr;
	eastl::unique_ptr<ConstantBuffer> injectLightingCB = nullptr;
	eastl::unique_ptr<ConstantBuffer> debugCB = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> voxelizeCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> voxelArgsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> injectLightCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> debugCompute = nullptr;

	eastl::unique_ptr<StructuredBuffer> meshBuffer = nullptr;

	eastl::unique_ptr<StructuredBuffer> nodeBuffer = nullptr;
	eastl::unique_ptr<Buffer> nodeCountBuffer = nullptr;
	eastl::unique_ptr<StructuredBuffer> voxelBuffer = nullptr;
	eastl::unique_ptr<Buffer> voxelCountBuffer = nullptr;

	eastl::unique_ptr<Buffer> voxelArgsBuffer = nullptr;

	eastl::unique_ptr<StructuredBuffer> lightBuffer = nullptr;

	eastl::unique_ptr<Texture3D> lod0Tex = nullptr;
	winrt::com_ptr<ID3D11SamplerState> lod0Sampler = nullptr;

	eastl::unique_ptr<Texture2D> prevDepth = nullptr;
	eastl::unique_ptr<Texture2D> debugTexture = nullptr;
};