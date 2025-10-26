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
#include "ShaderCache.h"
#include "Globals.h"
#include "State.h"

using namespace magic_enum::bitwise_operators;

namespace ShaderUtils
{
	template <typename T>
	struct ShaderProfile;

	template <>
	struct ShaderProfile<ID3D11VertexShader>
	{
		static constexpr const char* Profile = "vs_5_0";
	};

	template <>
	struct ShaderProfile<ID3D11PixelShader>
	{
		static constexpr const char* Profile = "ps_5_0";
	};

	template <>
	struct ShaderProfile<ID3D11GeometryShader>
	{
		static constexpr const char* Profile = "gs_5_0";
	};

	template <>
	struct ShaderProfile<ID3D11ComputeShader>
	{
		static constexpr const char* Profile = "cs_5_0";
	};

	template <typename T>
	struct PipelineShader
	{
		PipelineShader(const std::filesystem::path& path, const std::map<std::string, std::string>& defines)
		{
			std::vector<std::string> names;
			std::vector<std::string> values;
			std::vector<std::pair<const char*, const char*>> definesChar;

			names.reserve(defines.size());
			values.reserve(defines.size());
			definesChar.reserve(defines.size());

			for (const auto& [name, value] : defines) {
				names.push_back(name);
				values.push_back(value);
				definesChar.emplace_back(names.back().c_str(), values.back().c_str());
			}

			VoxelConeTracingGI::CompileShader(&shader, path.c_str(), ShaderProfile<T>::Profile, definesChar);
		}

		winrt::com_ptr<T> Get() const
		{
			return shader;
		}

		winrt::com_ptr<T> shader = nullptr;
	};

	inline std::string format_map(const std::map<std::string, std::string>& m)
	{
		std::string result = "{";
		bool first = true;
		for (const auto& [k, v] : m) {
			if (!first)
				result += ", ";
			result += std::format("\"{}\": \"{}\"", k, v);
			first = false;
		}
		result += "}";
		return result;
	}

	template <typename T>
	struct ShaderPermutations
	{
		ShaderPermutations() {}
		ShaderPermutations(const std::filesystem::path& path, const std::vector<std::pair<const char*, const char*>>& defines, const std::vector<std::pair<const char*, const char*>>& constDefines = {})
		{
			const size_t defineCount = defines.size();
			const size_t combinations = static_cast<size_t>(1) << defineCount;

			for (size_t mask = 0; mask < combinations; ++mask) {
				std::map<std::string, std::string> activeDefinesKey;
				std::map<std::string, std::string> activeDefines;

				for (size_t i = 0; i < defineCount; ++i) {
					if (mask & (static_cast<size_t>(1) << i)) {
						activeDefinesKey.emplace(defines[i].first, defines[i].second);
						activeDefines.emplace(defines[i].first, defines[i].second);
					}
				}

				// Defines that are always enabled and do not become a "key"
				for (const auto& constDefine : constDefines) {
					activeDefines.emplace(constDefine.first, constDefine.second);
				}

				//logger::info("Creating shader with active defines: {}", format_map(activeDefines));
				logger::info("Active defines: {}", format_map(activeDefines));

				shaders.emplace(activeDefinesKey, PipelineShader<T>(path, activeDefines));
			}
		}

		winrt::com_ptr<T> Get(const std::map<std::string, std::string>& defines) const
		{
			auto it = shaders.find(defines);

			if (it != shaders.end())
				return it->second.Get();

			return nullptr;
		}

		std::map<std::map<std::string, std::string>, PipelineShader<T>> shaders;
	};

	template <typename Inserter>
	void CollectShaderDefines(std::span<D3D_SHADER_MACRO> defines, Inserter inserter)
	{
		for (const auto& define: defines) {
			if (define.Name == nullptr)
				break;

			inserter(define.Name, define.Definition == nullptr ? "" : define.Definition);
		}
	}
};

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
			"Voxel Cone Tracing approximates global illumination by tracing cones in a 3D texture instead of using expensive rays, it is fast but memory intensive.",
			{
				"World space indirect lighting.",
				"Diffuse and Specular .",
				"Support for multiple bounces.",
				"Per-lighting source settings.",
				"Interior and exterior specific quality/performance settings."
			}
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return false; }
	virtual inline std::string_view GetShaderDefineName() override { return "VCTGI"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Lighting; };

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	void SetupVolumeTextures();
	void SetupScreenTextures();
	void SetupBuffers();
	void SetupViewport();

	void CompileShaders();

	void CompileStaticShaders();
	void ClearStaticShaders();

	void CompileVCTShader();
	void ClearVCTShader();
	//void GenerateCones();

	template <typename T>
	static void CompileShader(winrt::com_ptr<T>* programPtr, const wchar_t* path, const char* programType, std::vector<std::pair<const char*, const char*>> defines = {}, const std::vector<D3D11_INPUT_ELEMENT_DESC>& inputDesc = {}, winrt::com_ptr<ID3D11InputLayout>* inputLayoutPtr = nullptr);

	// Do stuff
	void Debug();
	void Main_RenderWorldAfter();

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	void BSShader_SetupGeometry(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
	void BSShader_RestoreGeometry(RE::BSShader* This, RE::BSRenderPass* Pass);

	void Main_RenderPlayerView(void* a1, bool a2, bool a3);
	void Main_RenderWorld(bool a1);
	void BSBatchRenderer_RenderPassImmediately(RE::BSRenderPass * a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);

	void InjectLighting();
	void ScreenConeTrace();
	void DrawVCTGI();

	void DebugDrawVoxels();

	void BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data);

	uint32_t GetVertexDescriptor(const RE::BSShader::Type& shaderType);
	uint32_t GetPixelDescriptor(const RE::BSShader::Type& shaderType);

	std::vector<float4> GenerateFibonacciCosineHemisphereSamples(int n);

	template <typename T>
	std::vector<std::pair<T, T>> GetShaderDefines(const RE::BSShader::Type& type, const uint32_t& descriptor);

	static constexpr const char* resolutionsLabels[6] = { "16", "32", "64", "128", "256", "512" };
	static constexpr uint16_t resolutions[6] = { 16, 32, 64, 128, 256, 512 };
	static constexpr uint MAX_LIGHTS = 1024;
	static constexpr eastl_size_t MAX_HISTORY = 60 * 60 * 60;
	static constexpr uint MAX_CLIPMAPS = 8;
	static constexpr uint MAX_UAV_CLIPMAPS = 2;

	bool queuedReset;
	bool renderingWorld;
	uint lastUpdateFrame = 0;
	uint padding[2] = {};

	enum class ChangedSetting : uint32_t
	{
		None = 0,
		VolumeResolution = 1 << 0,
		ConeCount = 1 << 1,
		ScreenResolution = 1 << 2,
		ClipmapCount = 1 << 3,
	};

	ChangedSetting changedSettings = ChangedSetting::None;

	enum DebugDrawMode : uint8_t
	{
		None,
		Albedo,
		Emission,
		Normal,
		Lighting,
		Final,
		Count
	};

	enum class VoxelMode : uint8_t
	{
		Standard,
		SphericalHarmicsL1,
		//SphericalHarmicsL2,
		Count
	};

	// Controls influence of injected lighting
	struct LightingSettings
	{
		float Direct = 1.0f;
		float Directional = 1.0f;
		float Omni = 1.0f;
		float Emissive = 1.0f;
		float Ambient = 1.0f;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(LightingSettings, Direct, Directional, Omni, Emissive, Ambient)
	};

	struct alignas(16) StepSettings
	{
		float Length = 1.0f;
		float Radius = 1.0f;
		float MipScale = 1.0f;
		float Alpha = 1.0f;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(StepSettings, Length, Radius, MipScale, Alpha)
	};
	static_assert(sizeof(StepSettings) % 16 == 0);

	struct ConeSettings
	{
		StepSettings Step;
		int Count = 5;
		int MinAngle = 0;
		int Angle = 60;
		int Distance = 75;
		float Alpha = 0.95f;
		float Offset = 1.0f;
		float Strength = 1.0f;
		uint Pad0;
		uint Pad1;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ConeSettings, Count, Step, MinAngle, Angle, Distance, Alpha, Offset, Strength)
	};
	static_assert(sizeof(ConeSettings) % 16 == 0);

	// Controls tracing, heavy performance/quality impact - tune responsibly
	struct alignas(16) TraceSettings
	{
		ConeSettings Diffuse;
		ConeSettings Specular;
		uint ResolutionMode = 0;
		uint AdditionalBounces = 0;
		uint Pad0;
		uint Pad1;
		uint Pad2;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TraceSettings, Diffuse, Specular, ResolutionMode, AdditionalBounces)
	};
	static_assert(sizeof(TraceSettings) % 16 == 0);

	// For Interior/Exterior specific tuning
	struct EnvironmentSettings
	{ 
		LightingSettings Lighting;
		TraceSettings Tracing;

		NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EnvironmentSettings, Lighting, Tracing)
	};

	struct Settings
	{
		uint8_t Enabled = true;
		VoxelMode Mode = VoxelMode::Standard;
		uint16_t Resolution = 64;
		int	Size = 20;
		int ClipmapCount = 4;
		int ScaleFactor = 2;
		uint8_t UpdateRate = 1;
		uint8_t ForceUpdate = true;
		EnvironmentSettings Interior;
		EnvironmentSettings Exterior;
		DebugDrawMode DebugDrawMode = DebugDrawMode::None;
	} settings;

	void DrawGeneralSettings();
	void DrawEnvironmentSettings(EnvironmentSettings& environmentSettings, const char* environmentName);
	void DrawConeSettings(ConeSettings& coneSettings, bool diffuse);

	size_t Statistics3DTextureMemory();
	size_t StatisticsBufferMemory();

	bool Enabled() const
	{
		return (bool)settings.Enabled;
	}

	bool Update() const
	{
		if (settings.UpdateRate == 0)
			return false;

		return globals::state->frameCount > lastUpdateFrame + (settings.UpdateRate - 1);
	}

	EnvironmentSettings CurrentEnvironmentSettings() const 
	{
		if (Util::IsInterior())
			return settings.Interior;
		else
			return settings.Exterior;	
	}

	uint NodeCount(uint maxDepth)
	{
		return static_cast<uint>(round((pow(8, maxDepth + 1) - 1) / 7.0));
	}

	uint LeafNodeCount(uint maxDepth)
	{
		return static_cast<uint>(pow(8, maxDepth));
	}

	uint OctreeMaxDepth() {
		uint lodResolution = settings.Resolution;
		return static_cast<uint>(ceil(log2(lodResolution)));
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

	struct alignas(4) uint3
	{
		uint x;
		uint y;
		uint z;
	};
	static_assert(sizeof(uint3) == 12);

	struct alignas(4) Voxel
	{
		uint3 Coord;
		float3 Albedo;
		float3 Normal;
		float3 Emissive;
	};
	static_assert(sizeof(Voxel) % 16 == 0);

    struct alignas(16)  Clipmap
	{
		float3 Min;
		float Size;
		float3 Max;
		float SizeRcp;
	};
	static_assert(sizeof(Clipmap) % 16 == 0);

	struct alignas(16) VoxelConeTracingGICB
	{
		uint Res;
		float ResRcp;
		uint ClipmapCount;
		uint Pad0;
		Clipmap Clipmaps[8];
	} voxelConeTracingGICBData;
	static_assert(sizeof(VoxelConeTracingGICB) % 16 == 0);

	struct alignas(16) ClipmapCB
	{
		uint Index;
		uint Start;
		uint Pad1;
		uint Pad2;
	} clipmapCBData;
	static_assert(sizeof(ClipmapCB) % 16 == 0);

	VoxelConeTracingGI::VoxelConeTracingGICB GetCommonBufferData() const;

	struct alignas(16) VSFrameBufferPSCB
	{
		float4 CameraPosAdjust;
	} vsFrameBufferPSData;
	static_assert(sizeof(VSFrameBufferPSCB) % 16 == 0);

	struct Light
	{
		float3 lvector;
		float range;
		float3 color;
		uint type;
	};
	static_assert(sizeof(Light) % 4 == 0);

	struct alignas(16) InjectLightingCB
	{
		uint LightCount;
		float EmissiveMult;
		float AmbientMult;
		float Pad0;
	} injectLightingCBData;
	static_assert(sizeof(InjectLightingCB) % 16 == 0);

	struct alignas(16) ScreenTraceCB
	{
		float4 NDCToView;
		float2 RcpFrameDim;
		float MaxMipLevel;
		float Pad0;
		float4 Cones[32];
	} screenTraceCBData;
	static_assert(sizeof(ScreenTraceCB) % 16 == 0);

	eastl::vector<Light> lights = {};

	eastl::unique_ptr<ConstantBuffer> voxelConeTracingGICB = nullptr;

	eastl::unique_ptr<ConstantBuffer> vsFrameBufferPSCB = nullptr;
	eastl::unique_ptr<ConstantBuffer> clipmapCB = nullptr;

	eastl::unique_ptr<ConstantBuffer> injectLightingCB = nullptr;

	eastl::unique_ptr<ConstantBuffer> coneTraceCB = nullptr;
	eastl::unique_ptr<ConstantBuffer> screenTraceCB = nullptr;
	
	std::unordered_map<RE::BSShader::Type, ShaderUtils::ShaderPermutations<ID3D11VertexShader>> voxelizeVertex;
	winrt::com_ptr<ID3D11GeometryShader> voxelizeGeometry = nullptr;
	std::unordered_map<RE::BSShader::Type, ShaderUtils::ShaderPermutations<ID3D11PixelShader>> voxelizePixel;

	winrt::com_ptr<ID3D11VertexShader> drawVertex = nullptr;
	winrt::com_ptr<ID3D11GeometryShader> drawGeometry = nullptr;
	std::array<winrt::com_ptr<ID3D11PixelShader>, DebugDrawMode::Count> drawPixelVariants;

	winrt::com_ptr<ID3D11VertexShader> accumulateVertex = nullptr;
	winrt::com_ptr<ID3D11PixelShader> accumulatePixel = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> indirectArgsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> postProcessCompute = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> voxelArgsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> injectLightCompute = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> vctGIInteriorCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> vctGIExteriorCompute = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> debugCompute = nullptr;
	
	winrt::com_ptr<ID3D11Device3> device3 = nullptr;

	winrt::com_ptr<ID3D11RasterizerState2> voxelRasterState = nullptr;
	D3D11_VIEWPORT voxelizeViewport = {};

	eastl::vector<eastl::unique_ptr<Buffer>> clipmapsVoxelMaskBuffer;
	eastl::vector<eastl::unique_ptr<Buffer>> clipmapsVoxelMapBuffer;
	eastl::unique_ptr<Buffer> voxelCountBuffer = nullptr;
	eastl::vector<eastl::unique_ptr<StructuredBuffer>> clipmapsVoxelBuffer;

	eastl::unique_ptr<Buffer> voxelIndirectArgsBuffer = nullptr;

	// Drawing voxels
	eastl::unique_ptr<Buffer> voxelDrawIndirectArgsBuffer = nullptr;
	eastl::unique_ptr<Buffer> voxelDrawVertexBuffer = nullptr;
	std::vector<D3D11_INPUT_ELEMENT_DESC> voxelInputDesc;
	winrt::com_ptr<ID3D11InputLayout> voxelDrawInputLayout = nullptr;
	winrt::com_ptr<ID3D11DepthStencilState> voxelDrawDSS = nullptr;

	eastl::unique_ptr<StructuredBuffer> lightBuffer = nullptr;

	eastl::vector<eastl::unique_ptr<Texture3D>> clipmapsVolumeTexture;
	winrt::com_ptr<ID3D11SamplerState> volumeSampler = nullptr;

	eastl::unique_ptr<Texture2D> diffuseGITexture = nullptr;
	eastl::unique_ptr<Texture2D> specularGITexture = nullptr;

	eastl::unique_ptr<Texture2D> debugColorTexture = nullptr;
	eastl::unique_ptr<Texture2D> debugDepthTexture = nullptr;

	inline auto GetOutputTextures()
	{
		return (loaded && settings.Enabled) ? std::make_tuple(diffuseGITexture->srv.get(), specularGITexture->srv.get()) : std::make_tuple(nullptr, nullptr);
	}

	eastl::hash_set<RE::BSTriShape*> cachedTriShapes;
	eastl::hash_set<RE::BSTriShape*> queuedTriShapes;
	eastl::hash_set<RE::BSGeometry*> unCulledGeometry;

	struct History
	{
		const std::chrono::steady_clock::time_point time;
		const long rendered;
	};

	std::deque<History> history;
	long rendered = 0;

	struct Hooks
	{
		template <RE::BSShader::Type ShaderType>
		struct BSShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				//if (globals::features::voxelConeTracingGI.loaded && globals::features::voxelConeTracingGI.Enabled())
				
				globals::features::voxelConeTracingGI.BSShader_SetupGeometry(This, Pass, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <RE::BSShader::Type ShaderType>
		struct BSShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				if (globals::features::voxelConeTracingGI.loaded && globals::features::voxelConeTracingGI.Enabled())
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

		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
			{
				globals::features::voxelConeTracingGI.BSBatchRenderer_RenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			//stl::detour_thunk<Main_RenderPlayerView>(REL::RelocationID(35560, 36559));
			stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142));

			stl::write_vfunc<0x6, BSShader_SetupGeometry<RE::BSShader::Type::Lighting>>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x6, BSShader_SetupGeometry<RE::BSShader::Type::Effect>>(RE::VTABLE_BSEffectShader[0]);

			stl::write_vfunc<0x7, BSShader_RestoreGeometry<RE::BSShader::Type::Lighting>>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x7, BSShader_RestoreGeometry<RE::BSShader::Type::Effect>>(RE::VTABLE_BSEffectShader[0]);

			if (REL::Module::IsAE()) {
				stl::write_vfunc<0x31, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			} else {
				stl::write_vfunc<0x30, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			}

			stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

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