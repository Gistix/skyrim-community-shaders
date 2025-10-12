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

#include "PCH.h"

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

	// Do stuff
	void Voxelize();
	void VoxelArgs();
	void InjectLighting();
	void Debug();
	void DrawVoxels();
	void Main_RenderWorldAfter();

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	void BSShader_SetupGeometry(RE::BSShader* This, RE::BSRenderPass* Pass);
	void BSShader_RestoreGeometry(RE::BSShader* This, RE::BSRenderPass* Pass);

	void Main_RenderPlayerView(void* a1, bool a2, bool a3);
	void Main_RenderWorld(bool a1);

	void PostProcess();

	void BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data);

	const char* dimensionsLabels[8] = { "4", "8", "16", "32", "64", "128", "256", "512" };
	const uint dimensions[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };
	static constexpr uint MAX_LIGHTS = 1024;
	float3 center;
	bool queuedReset;

	enum VoxelDrawMode {
		None,
		Albedo,
		Emission,
		Normal,
		Lighting,
		Final,
		Count
	};

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		uint EnableVoxelConeTracingGI = true;
		uint NoIndoorDir = false;
		int Lod0Resolution = 4;
		int Lod0Size = 20;
		uint UpdateVoxels = true;
		uint ForceUpdate = false;
		VoxelDrawMode VoxelDrawMode = None;
		uint _Pad1;
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

	struct uint3
	{
		uint x;
		uint y;
		uint z;
	};
	static_assert(sizeof(uint3) % 4 == 0);

	struct alignas(16) Voxel
	{
		uint3 Coord;
		float3 Albedo;
		float3 Emission;
		float3 Normal;
	};
	static_assert(sizeof(Voxel) % 16 == 0);

	struct Light
	{
		float3 lvector;
		float range;		
		float3 color;
		uint type;
	};
	static_assert(sizeof(Light) % 4 == 0);

	struct alignas(16) VoxelConeTracingGICB
	{
		float3 Min;
		float Size;
		float SizeInv;
		uint Res;
		float ResInv;
		float VoxelSize;
	} voxelConeTracingGICBData;
	static_assert(sizeof(VoxelConeTracingGICB) % 16 == 0);

	VoxelConeTracingGI::VoxelConeTracingGICB GetCommonBufferData() const;

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

	eastl::unique_ptr<ConstantBuffer> voxelConeTracingGICB = nullptr;
	eastl::unique_ptr<ConstantBuffer> injectLightingCB = nullptr;
	eastl::unique_ptr<ConstantBuffer> debugCB = nullptr;

	winrt::com_ptr<ID3D11VertexShader> voxelizeVertex = nullptr;
	winrt::com_ptr<ID3D11GeometryShader> voxelizeGeometry = nullptr;
	winrt::com_ptr<ID3D11PixelShader> voxelizePixel = nullptr;

	winrt::com_ptr<ID3D11VertexShader> drawVertex = nullptr;
	//winrt::com_ptr<ID3D11PixelShader> drawPixel = nullptr;
	std::array<winrt::com_ptr<ID3D11PixelShader>, VoxelDrawMode::Count> drawPixelVariants;

	winrt::com_ptr<ID3D11VertexShader> accumulateVertex = nullptr;
	winrt::com_ptr<ID3D11PixelShader> accumulatePixel = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> voxelizeCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> voxelArgsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> injectLightCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> debugCompute = nullptr;
	
	winrt::com_ptr<ID3D11Device3> device3 = nullptr;

	winrt::com_ptr<ID3D11RasterizerState2> voxelRasterState = nullptr;
	D3D11_VIEWPORT lod0Viewport = {};

	eastl::unique_ptr<StructuredBuffer> voxelAppendBuffer = nullptr;
	eastl::unique_ptr<Buffer> voxelCountBuffer = nullptr;

	// Multi purpose buffer for indirect arguments
	eastl::unique_ptr<Buffer> voxelIndirectArgsBuffer = nullptr;

	eastl::unique_ptr<StructuredBuffer> voxelBuffer = nullptr;

	// Post-Processing voxelization
	eastl::unique_ptr<Texture2DArray> voxelAccumulator = nullptr;
	D3D11_VIEWPORT accumulateViewport = {};
	winrt::com_ptr<ID3D11BlendState> accumulateBlendState = nullptr;
	std::vector<D3D11_INPUT_ELEMENT_DESC> accumulateInputDesc;
	winrt::com_ptr<ID3D11InputLayout> accumulateInputLayout = nullptr;

	// Drawing voxels
	eastl::unique_ptr<Buffer> voxelDrawVertexBuffer = nullptr;
	eastl::unique_ptr<Buffer> voxelDrawInstanceBuffer = nullptr;
	std::vector<D3D11_INPUT_ELEMENT_DESC> voxelInputDesc;
	winrt::com_ptr<ID3D11InputLayout> voxelDrawInputLayout = nullptr;
	winrt::com_ptr<ID3D11DepthStencilState> voxelDrawDSS = nullptr;

	eastl::unique_ptr<Buffer> voxelArgsBuffer = nullptr;

	eastl::unique_ptr<StructuredBuffer> lightBuffer = nullptr;

	eastl::unique_ptr<Texture3D> lod0Tex = nullptr;
	winrt::com_ptr<ID3D11SamplerState> lod0Sampler = nullptr;

	eastl::unique_ptr<Texture2D> prevDepth = nullptr;
	eastl::unique_ptr<Texture2D> debugTex = nullptr;

	eastl::hash_set<RE::BSGraphics::TriShape*> cachedTriShapes;
	eastl::hash_set<RE::BSGraphics::TriShape*> queuedTriShapes;

	struct History
	{
		const std::chrono::steady_clock::time_point time;
		const long rendered;
	};

	eastl::vector<History> history;
	long rendered = 0;

	struct Hooks
	{
		template <RE::BSShader::Type ShaderType>
		struct BSShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				if (globals::features::voxelConeTracingGI.loaded)
					globals::features::voxelConeTracingGI.BSShader_SetupGeometry(This, Pass);

				func(This, Pass, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <RE::BSShader::Type ShaderType>
		struct BSShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				if (globals::features::voxelConeTracingGI.loaded)
					globals::features::voxelConeTracingGI.BSShader_RestoreGeometry(This, Pass);

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

		struct Main_RenderPlayerView
		{
			static void thunk(void* a1, bool a2, bool a3)
			{
				globals::features::voxelConeTracingGI.Main_RenderPlayerView(a1, a2, a3);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				globals::features::voxelConeTracingGI.Main_RenderWorld(a1);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::detour_thunk<Main_RenderPlayerView>(REL::RelocationID(35560, 36559));
			stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142));

			stl::write_vfunc<0x6, BSShader_SetupGeometry<RE::BSShader::Type::Lighting>>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x7, BSShader_RestoreGeometry<RE::BSShader::Type::Lighting>>(RE::VTABLE_BSLightingShader[0]);

			if (REL::Module::IsAE()) {
				stl::write_vfunc<0x31, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			} else {
				stl::write_vfunc<0x30, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			}

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