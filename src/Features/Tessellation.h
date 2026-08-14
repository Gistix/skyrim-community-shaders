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
	virtual inline std::string_view GetShaderDefineName() override { return "TESSELLATION"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Lighting || t == RE::BSShader::Type::Utility; };

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;

	static void RestoreTechnique();
	static void SetupMaterial(RE::BSShader* a_shader, RE::BSShaderMaterial const* a_material);
	static void SetupGeometry(RE::BSShader* a_shader);
	static void RestoreGeometry();

	struct Hooks;

	////////////////////////////////////////////////// Feature Specific Data
	struct Settings
	{
		bool Enable = true;
		float Scale = 0.02f;
		float Factor = 2.0f;
		float Offset = 0.5f;
		float FadeStart = 30.0f;
		float FadeDistance = 20.0f;
	} settings;

	struct CbData
	{
		float Scale;
		float Factor;
		float Offset;
		float FadeStart;
		float FadeDistance;
		uint Pad0;
		uint Pad1;
		uint Pad2;
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

	std::unordered_map<uint64_t, TessellationStage> stageCache;  // key = shaderType << 32 | descriptor

	/** @brief Set per pass in the SetupMaterial hooks: the current material actually has displacement. */
	bool currentPassHasDisplacement = false;

	eastl::unique_ptr<ConstantBuffer> tessellationCb = nullptr;
	winrt::com_ptr<ID3D11SamplerState> parallaxSampler = nullptr;
};