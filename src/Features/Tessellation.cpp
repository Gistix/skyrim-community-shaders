#include "Tessellation.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

#include <RE/B/BSLightingShaderMaterialParallax.h>
#include <RE/B/BSLightingShaderMaterialParallaxOcc.h>

#include <vector>

#define I18N_KEY_PREFIX "feature.tessellation."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Tessellation::Settings,
	Enable,
	Scale,
	Factor,
	Offset,
	FadeStart,
	FadeDistance,
	Density)

////////////////////////////////////////////////////////////////////////////////////
void Tessellation::RestoreDefaultSettings()
{
	settings = {};
}

void Tessellation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Tessellation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Tessellation::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("tessellation_enable"), "Enable"), &settings.Enable);
	ImGui::SliderFloat(T(TKEY("tessellation_scale"), "Scale"), &settings.Scale, 0.0f, 1.0f, "%.4f");
	ImGui::SliderFloat(T(TKEY("tessellation_factor"), "Factor"), &settings.Factor, 1.0f, 32.0f, "%.1f");
	ImGui::SliderFloat(T(TKEY("tessellation_offset"), "Offset"), &settings.Offset, 0.0f, 1.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);

	ImGui::DragFloat(T(TKEY("tessellation_fade_start"), "Fade Start"), &settings.FadeStart, 0.1f, 0.0f, 10000.0f, "%.5f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::DragFloat(T(TKEY("tessellation_fade_distance"), "Fade Distance"), &settings.FadeDistance, 0.1f, 0.0f, 10000.0f, "%.5f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tessellation_density"), "Density"), &settings.Density, 0.0f, 2000.0f, "%.1f");
}

Tessellation::TessellationStage Tessellation::GetStage(const RE::BSShader& shader, uint32_t descriptor)
{
	// The cache is shared between the Lighting and Utility permutations, so
	// the key carries the shader type (their descriptor spaces collide).
	const uint64_t key = (static_cast<uint64_t>(shader.shaderType.get()) << 32) | descriptor;
	if (auto it = stageCache.find(key); it != stageCache.end())
		return it->second;

	std::array<D3D_SHADER_MACRO, 64> defines{};
	SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });

	std::vector<std::pair<const char*, const char*>> macroPairs;
	for (const auto& macro : defines) {
		if (!macro.Name)
			break;
		macroPairs.emplace_back(macro.Name, macro.Definition);
	}

	const bool isUtility = shader.shaderType.get() == RE::BSShader::Type::Utility;
	const wchar_t* hullPath = isUtility ? L"Data\\Shaders\\Tessellation\\UtilityHS.hlsl" : L"Data\\Shaders\\Tessellation\\LightingHS.hlsl";
	const wchar_t* domainPath = isUtility ? L"Data\\Shaders\\Tessellation\\UtilityDS.hlsl" : L"Data\\Shaders\\Tessellation\\LightingDS.hlsl";

	TessellationStage stage;
	if (auto rawPtr = reinterpret_cast<ID3D11HullShader*>(Util::CompileShader(hullPath, macroPairs, "hs_5_0")); rawPtr)
		stage.hull.attach(rawPtr);
	else
		logger::error("[Tessellation] Failed to compile hull shader for descriptor {:X}", descriptor);

	if (auto rawPtr = reinterpret_cast<ID3D11DomainShader*>(Util::CompileShader(domainPath, macroPairs, "ds_5_0")); rawPtr)
		stage.domain.attach(rawPtr);
	else
		logger::error("[Tessellation] Failed to compile domain shader for descriptor {:X}", descriptor);

	stageCache.emplace(key, stage);
	return stage;
}

void Tessellation::SetupMaterial(RE::BSShader* a_shader, RE::BSShaderMaterial const* a_material)
{
	auto& tessellation = globals::features::tessellation;

	if (!tessellation.settings.Enable)
		return;

	tessellation.currentPassHasDisplacement = false;

	if (a_shader->shaderType.all(RE::BSShader::Type::Utility)) {
		using Flags = RE::BSUtilityShader::Flags;

		auto* utilityShader = reinterpret_cast<RE::BSUtilityShader*>(a_shader);
		const auto flags = stl::enumeration<Flags>(static_cast<Flags>(utilityShader->unk90));
		
		if (flags.none(Flags::RenderDepth, Flags::RenderShadowmap, Flags::RenderShadowmap, Flags::RenderShadowmapPb))
			return;
	}

	RE::NiSourceTexture* heightTexture = nullptr;
	int32_t clampMode = 0;

	if (typeid(*a_material) == typeid(BSLightingShaderMaterialPBR)) {
		auto* pbrMaterial = reinterpret_cast<BSLightingShaderMaterialPBR const*>(a_material);
		heightTexture = pbrMaterial->displacementTexture.get();
		clampMode = pbrMaterial->textureClampMode;
	} else {
		const auto feature = a_material->GetFeature();
		if (feature == RE::BSShaderMaterial::Feature::kParallax || feature == RE::BSShaderMaterial::Feature::kParallaxOcc) {
			auto* parallaxMaterial = reinterpret_cast<RE::BSLightingShaderMaterialParallax const*>(a_material);
			heightTexture = parallaxMaterial->heightTexture.get();
			clampMode = parallaxMaterial->textureClampMode;
		}
	}

	const bool hasDisplacement = heightTexture != nullptr && heightTexture != globals::game::graphicsState->GetRuntimeData().defaultTextureBlack.get();
	if (!hasDisplacement)
		return;

	tessellation.currentPassHasDisplacement = true;

	auto context = globals::d3d::context;
	context->DSSetShaderResources(0, 1, &heightTexture->rendererTexture->resourceView);
}

/** @brief Binds the feature CB, HS/DS shaders and patch topology for a stage. */
static void ApplyTessellationStage(const Tessellation::TessellationStage& stage)
{
	auto& tessellation = globals::features::tessellation;
	auto context = globals::d3d::context;

	// Copy buffers from VS and PS
	{
		// PS PerMaterial (b1): ParallaxOccData at packoffset(c3)
		ID3D11Buffer* psB1 = nullptr;
		context->PSGetConstantBuffers(1, 1, &psB1);
		if (psB1) {
			context->DSSetConstantBuffers(1, 1, &psB1);
			psB1->Release();
		}

		// VS PerGeometry (b2): EyePosition at packoffset(c6)
		ID3D11Buffer* vsB2 = nullptr;
		context->VSGetConstantBuffers(2, 1, &vsB2);
		if (vsB2) {
			context->HSSetConstantBuffers(2, 1, &vsB2);
			context->DSSetConstantBuffers(2, 1, &vsB2);
			vsB2->Release();
		}

		// VS_PerFrame (b12): ViewProj at packoffset(c8)
		ID3D11Buffer* vsB12 = nullptr;
		context->VSGetConstantBuffers(12, 1, &vsB12);
		if (vsB12) {
			context->HSSetConstantBuffers(12, 1, &vsB12);
			context->DSSetConstantBuffers(12, 1, &vsB12);
			vsB12->Release();
		}

		auto* heightSampler = tessellation.parallaxSampler.get();
		context->DSSetSamplers(0, 1, &heightSampler);
	}

	auto buffer = tessellation.tessellationCb->CB();
	context->HSSetConstantBuffers(0, 1, &buffer);
	context->DSSetConstantBuffers(0, 1, &buffer);

	context->HSSetShader(stage.hull.get(), nullptr, 0);
	context->DSSetShader(stage.domain.get(), nullptr, 0);

	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
}

void Tessellation::RestoreTechnique()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	auto context = globals::d3d::context;
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->DSSetShaderResources(0, 1, &nullSRV);

	ID3D11SamplerState* nullSampler = nullptr;
	context->DSSetSamplers(0, 1, &nullSampler);

	ID3D11Buffer* nullBuffers[13] = { nullptr };
	context->HSSetConstantBuffers(0, 13, nullBuffers);
	context->DSSetConstantBuffers(0, 13, nullBuffers);
}

void Tessellation::SetupGeometry(RE::BSShader* a_shader)
{
	auto& tessellation = globals::features::tessellation;
	if (!tessellation.settings.Enable)
		return;

	if (!tessellation.currentPassHasDisplacement)
		return;

	// Key on the modified pixel descriptor: it retains the technique bits (the
	// vertex descriptor has them cleared for Parallax etc. by ModifyShaderLookup).
	// The Deferred bit is masked out since it does not affect HS/DS code.
	const auto descriptor = globals::state->modifiedPixelDescriptor & ~static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred);
	auto stage = tessellation.GetStage(*a_shader, descriptor);
	if (!stage.hull || !stage.domain) {
		logger::error("[Tessellation] Missing HS/DS stage for descriptor {:X}, pass renders vanilla", descriptor);
		return;
	}

	ApplyTessellationStage(stage);
}

void Tessellation::RestoreGeometry()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

struct Tessellation::Hooks
{
	struct Main_RenderDepth
	{
		static void thunk(bool a1, bool a2)
		{
			auto& tessellation = globals::features::tessellation;
			tessellation.tessellationData.Scale = tessellation.settings.Scale / Util::Units::GAME_UNIT_TO_M;
			tessellation.tessellationData.Factor = tessellation.settings.Factor;
			tessellation.tessellationData.Offset = tessellation.settings.Offset;
			tessellation.tessellationData.FadeStart = tessellation.settings.FadeStart / Util::Units::GAME_UNIT_TO_M;
			tessellation.tessellationData.FadeDistance = tessellation.settings.FadeDistance / Util::Units::GAME_UNIT_TO_M;
			tessellation.tessellationData.Density = tessellation.settings.Density;
			tessellation.tessellationCb->Update(tessellation.tessellationData);

			func(a1, a2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupMaterial
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSLightingShaderMaterialBase const* a_material)
		{
			func(a_shader, a_material);
			Tessellation::SetupMaterial(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreTechnique
	{
		static void thunk(RE::BSLightingShader* a_shader, uint32_t a_technique)
		{
			func(a_shader, a_technique);
			Tessellation::RestoreTechnique();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::SetupGeometry(a_shader);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::RestoreGeometry();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupMaterial
	{
		static void thunk(RE::BSUtilityShader* a_shader, RE::BSShaderMaterial const* a_material)
		{
			func(a_shader, a_material);
			Tessellation::SetupMaterial(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_RestoreTechnique
	{
		static void thunk(RE::BSUtilityShader* a_shader, uint32_t a_technique)
		{
			func(a_shader, a_technique);
			Tessellation::RestoreTechnique();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupGeometry
	{
		static void thunk(RE::BSUtilityShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::SetupGeometry(a_shader);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_RestoreGeometry
	{
		static void thunk(RE::BSUtilityShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::RestoreGeometry();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct DirtyStates_CreateInputLayout
	{
		// The game builds input layouts from the RendererData vertexDesc intersected with the shader's reflected vertexDesc. 
		// The depth prepass RendererData vertexDesc omits the TEXCOORD0 element bit, so the layout is position-only regardless of
		// the shader's input signature. Force the bit back in so tessellated utility passes get mesh UVs; the builder derives the element's byte
		// offset from the descriptor's own offset bits, which are retained.
		static ID3D11InputLayout* thunk(RE::BSGraphics::VertexDesc a_vertexDesc)
		{
			auto originalShader = globals::state->currentShader;
			auto originalVertexShader = *globals::game::currentVertexShader;

			const bool isUtility = originalShader && originalShader->shaderType.all(RE::BSShader::Type::Utility);
			if (isUtility) {
				if (!a_vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) {
					a_vertexDesc.SetFlag(RE::BSGraphics::Vertex::VF_UV);

					if (a_vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) == 0)
						a_vertexDesc.SetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0, 16);
				}

				auto shaderCache = globals::shaderCache;

				const bool asyncCompilation = shaderCache->IsAsync();
				if (asyncCompilation)
					shaderCache->SetAsync(false);

				// Override the original shader so the input layout gets TEXCOORD input from our custom shader
				auto customShader = shaderCache->GetVertexShader(*originalShader, globals::state->modifiedVertexDescriptor);

				if (asyncCompilation)
					shaderCache->SetAsync(true);

				*globals::game::currentVertexShader = customShader;
			}

			auto result = func(a_vertexDesc);

			if (isUtility)
				*globals::game::currentVertexShader = originalVertexShader; 

			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x3, BSLightingShader_RestoreTechnique>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x4, BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);

		stl::write_vfunc<0x3, BSUtilityShader_RestoreTechnique>(RE::VTABLE_BSUtilityShader[0]);
		stl::write_vfunc<0x4, BSUtilityShader_SetupMaterial>(RE::VTABLE_BSUtilityShader[0]);
		stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
		stl::write_vfunc<0x7, BSUtilityShader_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);

		// Injects TEXCOORD into Utility shader input layouts
		stl::detour_thunk<DirtyStates_CreateInputLayout>(REL::RelocationID(75583, 77389));

		// Updates constant buffer before depth is rendered
		stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));

		logger::info("[Tessellation] Installed hooks");
	}
};

void Tessellation::PostPostLoad()
{
	Hooks::Install();
}

void Tessellation::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		tessellationCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CbData>());
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, parallaxSampler.put()));
	}
}

void Tessellation::ClearShaderCache()
{
	stageCache.clear();
}
