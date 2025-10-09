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

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	void BSLightingShader_SetupVoxelization(RE::BSShader* This, RE::BSRenderPass* Pass);

	void BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data);
	void DrawTriShape(void* This, RE::BSGraphics::TriShape* GraphicsTriShape, uint32_t StartIndex, uint32_t Count);

	const char* dimensionsLabels[8] = { "4", "8", "16", "32", "64", "128", "256", "512" };
	const uint dimensions[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };
	static constexpr uint MAX_LIGHTS = 1024;
	float3 center;
	bool queuedReset;


	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		uint EnableVoxelConeTracingGI = true;
		uint NoIndoorDir = false;
		int Lod0Resolution = 4;
		int Lod0Size = 20;
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
				return dimensions[settings.Lod0Resolution];
			case 2:
				return dimensions[settings.Lod0Resolution];
			case 3:
				return dimensions[settings.Lod0Resolution];
			default:
				return dimensions[settings.Lod0Resolution];
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
		float4 NDCToView;
		float2 RcpFrameDim;
		float Cell2Coord;
		float Resolution;
		float3 Center;
		uint MaxDepth;
		float3 Size;
		uint _pad0;		
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

	winrt::com_ptr<ID3D11RasterizerState> voxelRasterState = nullptr;

	eastl::unique_ptr<StructuredBuffer> nodeBuffer = nullptr;
	eastl::unique_ptr<Buffer> nodeCountBuffer = nullptr;
	eastl::unique_ptr<StructuredBuffer> voxelBuffer = nullptr;
	eastl::unique_ptr<Buffer> voxelCountBuffer = nullptr;

	eastl::unique_ptr<Buffer> voxelArgsBuffer = nullptr;

	eastl::unique_ptr<StructuredBuffer> lightBuffer = nullptr;

	eastl::unique_ptr<Texture3D> lod0Tex = nullptr;
	winrt::com_ptr<ID3D11SamplerState> lod0Sampler = nullptr;

	eastl::unique_ptr<Texture2D> prevDepth = nullptr;
	eastl::unique_ptr<Texture2D> debugTex = nullptr;

	struct BufferData
	{
		const char* name;
		float4x4 transform;
		float3 min;
		float3 max;
		eastl::unique_ptr<Buffer> vertexBuffer;
		eastl::unique_ptr<Buffer> indexBuffer;
	};

	eastl::hash_set<RE::BSGraphics::TriShape*> queuedTriShapes;
	eastl::hash_map<RE::BSGraphics::TriShape*, BufferData> cachedTriShapes;

	struct History
	{
		const std::chrono::steady_clock::time_point& time;
		const eastl_size_t& rendered;
	};

	eastl::vector<History> history;
	eastl_size_t lastQueueSize = 0;

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				globals::features::voxelConeTracingGI.BSLightingShader_SetupVoxelization(This, Pass);
				func(This, Pass, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSTriShape_UpdateWorldData
		{
			static void thunk(RE::BSTriShape* This, RE::NiUpdateData* a_data)
			{
				globals::features::voxelConeTracingGI.BSTriShape_UpdateWorldData(This, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DrawTriShape
		{
			static void thunk(void* This, RE::BSGraphics::TriShape* GraphicsTriShape, uint32_t StartIndex, uint32_t Count) {
				func(This, GraphicsTriShape, StartIndex, Count);
				globals::features::voxelConeTracingGI.DrawTriShape(This, GraphicsTriShape, StartIndex, Count);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);

			if (REL::Module::IsAE()) {
				stl::write_vfunc<0x31, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			} else {
				stl::write_vfunc<0x30, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			}

			stl::detour_thunk<DrawTriShape>(REL::RelocationID(75477, 107175));

			logger::info("[VCTGI] Installed hooks");
		}
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;

			if (!ui) {
				logger::error("UI event source not found");
				return false;
			}

			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());

			return true;
		}
	};
};
static_assert(sizeof(VoxelConeTracingGI) % 16 == 0);