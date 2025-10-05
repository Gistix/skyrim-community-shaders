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
#include <shared_mutex>

struct VoxelConeTracingGI : public Feature
{
	static VoxelConeTracingGI* GetSingleton()
	{
		static VoxelConeTracingGI singleton;
		return &singleton;
	}

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

	const char* dimensionsLabels[8] = { "4", "8", "16", "32", "64", "128", "256", "512" };
	const uint dimensions[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };
	static constexpr uint MAX_LIGHTS = 1024;
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

#pragma region VoxelizationBuffer
	std::shared_mutex mutex;

	struct BufferData
	{
		winrt::com_ptr<ID3D11Resource> buffer;
		bool registered = false;
		uint width;
		uint index;
	};

	eastl::hash_map<ID3D11Buffer*, BufferData> vertexBuffers;
	eastl::hash_map<ID3D11Buffer*, BufferData> indexBuffers;
	
	struct InputLayoutData
	{
		uint32_t stride;
		uint32_t offset;
		DXGI_FORMAT format;
	};

	eastl::hash_map<ID3D11InputLayout*, InputLayoutData> inputLayouts;
	eastl::hash_map<uint64_t, ID3D11InputLayout*> vertexDescToInputLayout;

	struct InstanceData
	{
		float4x4 transform;
		float3 min;
		float3 max;
		bool visibleState = false;
	};

	eastl::hash_set<RE::BSTriShape*> queuedInstances;
	eastl::hash_map<RE::BSTriShape*, InstanceData> instances;

	BufferData AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData);

	void RegisterVertexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer);
	void RegisterIndexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer);
	void RegisterInputLayout(ID3D11InputLayout* ppInputLayout, D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements);

	void UnregisterVertexBuffer(ID3D11Buffer* ppBuffer);
	void UnregisterIndexBuffer(ID3D11Buffer* ppBuffer);

	void FrameUpdate();

	void InstallD3DHooks();

	void BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data);

	void AddInstance(RE::BSTriShape* a_geometry);

	winrt::com_ptr<ID3D11RasterizerState> voxelRasterState = nullptr;

	struct Hooks
	{
		struct ID3D11Device_CreateBuffer
		{
			static HRESULT thunk(ID3D11Device* This, const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
			{
				HRESULT hr = func(This, pDesc, pInitialData, ppBuffer);

				if (pInitialData) {

					if (pDesc->BindFlags & D3D11_BIND_VERTEX_BUFFER) {
						GetSingleton()->RegisterVertexBuffer(pDesc, pInitialData, ppBuffer);
					} else if (pDesc->BindFlags & D3D11_BIND_INDEX_BUFFER) {
						GetSingleton()->RegisterIndexBuffer(pDesc, pInitialData, ppBuffer);
					}
				}

				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ID3D11Device_CreateInputLayout
		{
			static HRESULT thunk(ID3D11Device* This, D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout)
			{
				HRESULT hr = func(This, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);

				if (pInputElementDescs && NumElements > 0) {
					GetSingleton()->RegisterInputLayout(*ppInputLayout, pInputElementDescs, NumElements);
				}

				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSTriShape_UpdateWorldData
		{
			static void thunk(RE::BSTriShape* This, RE::NiUpdateData* a_data)
			{
				GetSingleton()->BSTriShape_UpdateWorldData(This, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DirtyStates_CreateInputLayoutFromVertexDesc
		{
			static ID3D11InputLayout* thunk(uint64_t a_vertexDesc)
			{
				auto inputLayout = func(a_vertexDesc);
				GetSingleton()->vertexDescToInputLayout.insert({ a_vertexDesc, inputLayout });
				return inputLayout;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			//if (REL::Module::IsAE()) {
			//	stl::write_vfunc<0x31, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			//} else {
			//	stl::write_vfunc<0x30, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			//}
			stl::write_thunk_call<DirtyStates_CreateInputLayoutFromVertexDesc>(REL::RelocationID(75580, 75580).address() + REL::Relocate(0x465, 0x465));

			logger::info("[VCT] Installed hooks");
		}
	};

#pragma endregion

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
};