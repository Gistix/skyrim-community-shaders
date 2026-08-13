#pragma once

#include <mutex>
#include <unordered_map>

struct Tessellation : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "Tessellation"; }
	virtual inline std::string GetShortName() override { return "Tessellation"; }
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
	virtual void PostPostLoad() override;

	static void BSLightingShader_SetupMaterial(RE::BSLightingShader* a_shader, RE::BSLightingShaderMaterialBase const* a_material);
	static void BSLightingShader_RestoreTechnique();
	static void BSLightingShader_SetupGeometry(RE::BSShader* a_shader);
	static void BSLightingShader_RestoreGeometry();
	static void BSUtilityShader_SetupMaterial(RE::BSShader* a_shader, RE::BSShaderMaterial const* a_material);
	static void BSUtilityShader_SetupGeometry(RE::BSShader* a_shader);
	static void BSUtilityShader_RestoreTechnique();
	static void BSUtilityShader_RestoreGeometry();

	struct Hooks;

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		bool Enable = true;
		float TessellationScale = 0.02f;
		float TessellationFactor = 2.0f;
	} settings;

	struct CbData
	{
		float TessellationScale;
		float TessellationFactor;
		float _pad0;
		float _pad1;
	} tessellationData;
	static_assert(sizeof(CbData) % 16 == 0,
		"CbData must be aligned to 16 bytes. "
		"Check out maraneshi.github.io/HLSL-ConstantBufferLayoutVisualizer/ if you're unsure.");

	/** @brief Hull/domain shader pair for a single lighting permutation. */
	struct TessellationStage
	{
		winrt::com_ptr<ID3D11HullShader> hull;
		winrt::com_ptr<ID3D11DomainShader> domain;
	};

	/**
	 * @brief Returns (compiling on miss) the hull/domain pair for a lighting
	 * permutation. The descriptor is the modified pixel descriptor of the
	 * current pass: it retains the technique bits (the vertex descriptor has
	 * them cleared for Parallax etc. by State::ModifyShaderLookup), while all
	 * layout-affecting flags match between the two.
	 * @param shader The lighting shader (defines dispatch).
	 * @param descriptor The modified pixel descriptor of the current pass.
	 * @return The compiled stage; empty com_ptrs when compilation failed.
	 */
	TessellationStage GetStage(const RE::BSShader& shader, uint32_t descriptor);

	/**
	 * @brief Mirrors the game's per-pass constant buffers and the parallax
	 * heightmap SRV/sampler onto the HS/DS stages for PARALLAX / PARALLAX_OCC
	 * techniques.
	 * @param descriptor The modified pixel descriptor of the current pass.
	 * @return True when the heightmap was bound at slot 3 (tessellation
	 * enabled for the pass), false otherwise (pass renders vanilla).
	 */
	bool MirrorParallaxBindings();

	std::unordered_map<uint64_t, TessellationStage> stageCache;  // key = shaderType << 32 | descriptor

	/** @brief Set per pass in the SetupMaterial hooks: the current material actually has displacement. */
	bool currentPassHasDisplacement = false;

	/** @brief PS texture slot the heightmap is bound to (3 = vanilla parallax, 4 = PBR/utility). */
	uint32_t currentHeightmapSlot = 3;

	eastl::unique_ptr<ConstantBuffer> tessellationCb = nullptr;
	winrt::com_ptr<ID3D11SamplerState> parallaxSampler = nullptr;
};