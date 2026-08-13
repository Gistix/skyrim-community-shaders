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
	TessellationScale,
	TessellationFactor)

////////////////////////////////////////////////////////////////////////////////////

/** @brief True for the materials that actually tessellate (parallax displacement). */
static bool IsTessellatableTechnique(uint32_t descriptor)
{
	const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>(0x3F & (descriptor >> 24));
	if (technique == SIE::ShaderCache::LightingShaderTechniques::Parallax ||
		technique == SIE::ShaderCache::LightingShaderTechniques::ParallaxOcc) {
		return true;
	}
	// PBR materials (non-landscape): displacement texture bound at PS slot 4
	// (TruePBR.cpp, SetPSTexture(4, ...)). Landscape PBR uses per-tile t80+
	// displacement textures, out of scope for now.
	if ((descriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr)) != 0) {
		return technique == SIE::ShaderCache::LightingShaderTechniques::None ||
		       technique == SIE::ShaderCache::LightingShaderTechniques::TreeAnim;
	}
	return false;
}

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
	ImGui::SliderFloat(T(TKEY("tessellation_scale"), "Tessellation Scale"), &settings.TessellationScale, 0.0f, 0.1f, "%.4f");
	ImGui::SliderFloat(T(TKEY("tessellation_factor"), "Tessellation Factor"), &settings.TessellationFactor, 1.0f, 8.0f, "%.1f");
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

void Tessellation::BSLightingShader_SetupMaterial(RE::BSLightingShader* a_shader, RE::BSLightingShaderMaterialBase const* a_material)
{
	auto& tessellation = globals::features::tessellation;
	tessellation.currentPassHasDisplacement = false;

	// Vanilla parallax materials bind their heightmap at PS slot 3 in the
	// game's SetupMaterial; the SRV check in SetupGeometry confirms it.
	const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>(0x3F & (a_shader->currentRawTechnique >> 24));
	if (technique == SIE::ShaderCache::LightingShaderTechniques::Parallax ||
		technique == SIE::ShaderCache::LightingShaderTechniques::ParallaxOcc) {
		tessellation.currentPassHasDisplacement = true;
		tessellation.currentHeightmapSlot = 3;
		return;
	}

	// PBR materials: mirror TruePBR's flag-based cast (TruePBR.cpp:782-783)
	// and require an actual displacement texture, same as TruePBR.cpp:986.
	if ((a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr)) != 0) {
		auto* pbrMaterial = static_cast<BSLightingShaderMaterialPBR const*>(a_material);
		tessellation.currentPassHasDisplacement = pbrMaterial->displacementTexture != nullptr &&
			pbrMaterial->displacementTexture != globals::game::graphicsState->GetRuntimeData().defaultTextureBlack;
		tessellation.currentHeightmapSlot = 4;
	}
}

void Tessellation::BSUtilityShader_SetupMaterial([[maybe_unused]] RE::BSShader* a_shader, RE::BSShaderMaterial const* a_material)
{
	auto& tessellation = globals::features::tessellation;
	tessellation.currentPassHasDisplacement = false;

	auto* material = static_cast<RE::BSLightingShaderMaterialBase const*>(a_material);
	const auto feature = material->GetFeature();

	// Vanilla parallax materials carry the heightmap as heightTexture; the
	// utility pass never binds it, so bind it ourselves at PS slot 4 (the
	// utility descriptor carries no technique flags, so the utility DS
	// declares a single unconditional slot).
	RE::NiSourceTexture* heightTexture = nullptr;
	if (feature == RE::BSShaderMaterial::Feature::kParallax) {
		heightTexture = static_cast<RE::BSLightingShaderMaterialParallax const*>(material)->heightTexture.get();
	} else if (feature == RE::BSShaderMaterial::Feature::kParallaxOcc) {
		heightTexture = static_cast<RE::BSLightingShaderMaterialParallaxOcc const*>(material)->heightTexture.get();
	}
	if (heightTexture) {
		tessellation.currentPassHasDisplacement = true;
		tessellation.currentHeightmapSlot = 4;
		globals::game::shadowState->SetPSTexture(4, heightTexture->rendererTexture);
		globals::game::shadowState->SetPSTextureAddressMode(4, RE::BSGraphics::TextureAddressMode::kWrapSWrapT);
		globals::game::shadowState->SetPSTextureFilterMode(4, RE::BSGraphics::TextureFilterMode::kAnisotropic);
		return;
	}

	// PBR materials: identified via the global tracking map (membership check;
	// the cast is never dereferenced unless the map says it is a PBR material).
	auto* pbrCandidate = static_cast<BSLightingShaderMaterialPBR*>(const_cast<RE::BSShaderMaterial*>(a_material));
	if (BSLightingShaderMaterialPBR::All.contains(pbrCandidate)) {
		auto* pbr = static_cast<BSLightingShaderMaterialPBR const*>(a_material);
		if (pbr->displacementTexture && pbr->displacementTexture != globals::game::graphicsState->GetRuntimeData().defaultTextureBlack) {
			tessellation.currentPassHasDisplacement = true;
			tessellation.currentHeightmapSlot = 4;
			globals::game::shadowState->SetPSTexture(4, pbr->displacementTexture->rendererTexture);
			globals::game::shadowState->SetPSTextureAddressMode(4, static_cast<RE::BSGraphics::TextureAddressMode>(pbr->textureClampMode));
			globals::game::shadowState->SetPSTextureFilterMode(4, RE::BSGraphics::TextureFilterMode::kAnisotropic);
		}
	}
}

/** @brief Binds the feature CB, HS/DS shaders and patch topology for a stage. */
static void ApplyTessellationStage(const Tessellation::TessellationStage& stage)
{
	auto& tessellation = globals::features::tessellation;
	auto context = globals::d3d::context;

	auto buffer = tessellation.tessellationCb->CB();
	context->HSSetConstantBuffers(0, 1, &buffer);
	context->DSSetConstantBuffers(0, 1, &buffer);
	context->HSSetShader(stage.hull.get(), nullptr, 0);
	context->DSSetShader(stage.domain.get(), nullptr, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
}

/** @brief Unbinds the HS/DS shaders and the stage bindings applied by ApplyTessellationStage. */
static void UnbindTessellationStage()
{
	auto context = globals::d3d::context;
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);

	// Release the HS/DS stage bindings mirrored by MirrorParallaxBindings;
	// otherwise the context keeps references to the game's buffers and the
	// heightmap texture alive after the pass.
	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	context->DSSetShaderResources(3, 2, nullSRVs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->DSSetSamplers(3, 2, nullSamplers);
	ID3D11Buffer* nullBuffers[13] = { nullptr };
	context->HSSetConstantBuffers(0, 13, nullBuffers);
	context->DSSetConstantBuffers(0, 13, nullBuffers);
}

void Tessellation::BSLightingShader_RestoreTechnique()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	UnbindTessellationStage();
}

void Tessellation::BSUtilityShader_RestoreTechnique()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	UnbindTessellationStage();
}

void Tessellation::BSLightingShader_SetupGeometry(RE::BSShader* a_shader)
{
	auto& tessellation = globals::features::tessellation;
	if (!tessellation.settings.Enable)
		return;

	// Key on the modified pixel descriptor: it retains the technique bits (the
	// vertex descriptor has them cleared for Parallax etc. by ModifyShaderLookup).
	// The Deferred bit is masked out since it does not affect HS/DS code.
	const auto descriptor = globals::state->modifiedPixelDescriptor & ~static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred);
	if (!IsTessellatableTechnique(descriptor))
		return;

	// Only tessellate when the material actually has displacement (recorded in
	// the SetupMaterial hook); PBR materials without a displacement texture
	// render vanilla.
	if (!tessellation.currentPassHasDisplacement)
		return;

	auto stage = tessellation.GetStage(*a_shader, descriptor);
	if (!stage.hull || !stage.domain) {
		logger::error("[Tessellation] Missing HS/DS stage for descriptor {:X}, pass renders vanilla", descriptor);
		return;
	}

	if (!tessellation.MirrorParallaxBindings()) {
		logger::error("[Tessellation] No heightmap bound at PS slot {} for descriptor {:X}, pass renders vanilla", tessellation.currentHeightmapSlot, descriptor);
		return;
	}

	ApplyTessellationStage(stage);
}

void Tessellation::BSUtilityShader_SetupGeometry(RE::BSShader* a_shader)
{
	auto& tessellation = globals::features::tessellation;
	if (!tessellation.settings.Enable)
		return;

	// Only tessellate utility passes for displacement materials (depth prepass
	// matching the displaced lighting surface); the heightmap was bound in the
	// SetupMaterial hook.
	if (!tessellation.currentPassHasDisplacement)
		return;

	const auto descriptor = globals::state->modifiedPixelDescriptor & ~static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred);

	auto stage = tessellation.GetStage(*a_shader, descriptor);
	if (!stage.hull || !stage.domain) {
		logger::error("[Tessellation] Missing Utility HS/DS stage for descriptor {:X}, pass renders vanilla", descriptor);
		return;
	}

	if (!tessellation.MirrorParallaxBindings()) {
		logger::error("[Tessellation] No heightmap bound at PS slot {} for utility descriptor {:X}, pass renders vanilla", tessellation.currentHeightmapSlot, descriptor);
		return;
	}

	ApplyTessellationStage(stage);
}

bool Tessellation::MirrorParallaxBindings()
{
	// The heightmap slot is decided by the SetupMaterial hooks (3 for vanilla
	// parallax, 4 for PBR and the utility path); the descriptor does not
	// carry that information for utility passes.
	const uint32_t heightSlot = currentHeightmapSlot;

	auto context = globals::d3d::context;

	// Heightmap SRV: when the material has no parallax heightmap bound, skip
	// tessellation entirely and let the pass render vanilla.
	ID3D11ShaderResourceView* heightSRV = nullptr;
	context->PSGetShaderResources(heightSlot, 1, &heightSRV);
	if (!heightSRV)
		return false;
	context->DSSetShaderResources(heightSlot, 1, &heightSRV);
	heightSRV->Release();

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

	// Sampler mirrored from the PS stage; the PS has no PARALLAX_OCC path, so
	// fall back to our own sampler when none is bound.
	ID3D11SamplerState* heightSampler = nullptr;
	context->PSGetSamplers(heightSlot, 1, &heightSampler);
	if (heightSampler) {
		context->DSSetSamplers(heightSlot, 1, &heightSampler);
		heightSampler->Release();
	} else if (parallaxSampler) {
		heightSampler = parallaxSampler.get();
		context->DSSetSamplers(heightSlot, 1, &heightSampler);
	}

	return true;
}

void Tessellation::BSLightingShader_RestoreGeometry()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Tessellation::BSUtilityShader_RestoreGeometry()
{
	if (!globals::features::tessellation.currentPassHasDisplacement)
		return;

	globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

struct Tessellation::Hooks
{
	struct Main_RenderWorld
	{
		static void thunk(bool a1)
		{
			auto& tessellation = globals::features::tessellation;
			tessellation.tessellationData.TessellationScale = tessellation.settings.TessellationScale;
			tessellation.tessellationData.TessellationFactor = tessellation.settings.TessellationFactor;
			tessellation.tessellationCb->Update(tessellation.tessellationData);

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupMaterial
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSLightingShaderMaterialBase const* a_material)
		{
			func(a_shader, a_material);
			Tessellation::BSLightingShader_SetupMaterial(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreTechnique
	{
		static void thunk(RE::BSShader* a_shader, uint32_t a_technique)
		{
			func(a_shader, a_technique);
			Tessellation::BSLightingShader_RestoreTechnique();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::BSLightingShader_SetupGeometry(a_shader);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::BSLightingShader_RestoreGeometry();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupMaterial
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial const* a_material)
		{
			func(a_shader, a_material);
			Tessellation::BSUtilityShader_SetupMaterial(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_RestoreTechnique
	{
		static void thunk(RE::BSShader* a_shader, uint32_t a_technique)
		{
			func(a_shader, a_technique);
			Tessellation::BSUtilityShader_RestoreTechnique();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::BSUtilityShader_SetupGeometry(a_shader);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			Tessellation::BSUtilityShader_RestoreGeometry();
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
		
		stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841));

		logger::info("[Tessellation] Installed hooks - BSLightingShader SetupMaterial/RestoreTechnique/SetupGeometry/RestoreGeometry + BSUtilityShader SetupMaterial/SetupGeometry/RestoreTechnique/RestoreGeometry + Main_RenderWorld");
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
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
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