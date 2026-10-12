#pragma once

#include "Buffer.h"
#include "CreationEngineRaytracing.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include <winrt/base.h>

/**
 * @brief Regular feature providing hardware-accelerated Global Illumination.
 */
struct GlobalIllumination : Feature
{
public:
	// Metadata
	virtual std::string GetName() override { return "Global Illumination"; }
	virtual std::string GetDisplayName() override { return T("feature.global_illumination.name", "Global Illumination"); }
	virtual std::string GetShortName() override { return "GlobalIllumination"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return false; }
	virtual bool IsInMenu() const override { return true; }
	virtual std::string_view GetShaderDefineName() override { return "RTGI"; }
	virtual bool HasShaderDefine(RE::BSShader::Type t) override { return Available() && (t == RE::BSShader::Type::Lighting || t == RE::BSShader::Type::Grass); }
	virtual ReleaseStage GetReleaseStage() const override { return ReleaseStage::Release; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return std::make_pair(
			"Global Illumination simulates indirect lighting using hardware ray tracing and dynamic diffuse global illumination (DDGI).",
			std::vector<std::string>{
				"Dynamic Diffuse Global Illumination (DDGI)",
				"Real-time indirect bounce lighting",
				"Hardware-accelerated ray tracing" });
	}

	struct Settings
	{
		bool Enabled = true;
		CreationEngineRaytracing::RaytracingSettings RaytracingSettings;
		CreationEngineRaytracing::GeneralSettings GeneralSettings{
			.Mode = CreationEngineRaytracing::Mode::GlobalIllumination
		};
		CreationEngineRaytracing::NRDSettings NRDSettings;
		CreationEngineRaytracing::NRDReblurSettings NRDReblurSettings;
		CreationEngineRaytracing::NRDRelaxSettings NRDRelaxSettings;
		CreationEngineRaytracing::HairBSDF HairBSDF = CreationEngineRaytracing::HairBSDF::FarFieldBCSDF;
		CreationEngineRaytracing::SSSSettings SSSSettings;
		CreationEngineRaytracing::MaterialSettings MaterialSettings;
		CreationEngineRaytracing::LightingSettings LightingSettings;
		CreationEngineRaytracing::WaterSettings WaterSettings;
		CreationEngineRaytracing::SHaRCSettings SHaRCSettings;

		bool operator==(const Settings&) const = default;
	} settings;

	// Converted normal/roughness input shared with CERT (filled by ConvertTextures)
	winrt::com_ptr<ID3D11Texture2D> normalRoughnessTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> normalRoughnessSRV = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> normalRoughnessUAV = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> convertTexturesCS = nullptr;

	virtual void SetupResources() override;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	void DrawGeneralSettings();
	void DrawAdvancedSettings();
	void DrawNRDSettings();
	void DrawReblurSettings();
	void DrawRelaxSettings();
	void DrawHairBSDFSettings();
	void DrawSSSSettings();
	void DrawMaterialSettings();
	void DrawLightingSettings();
	void DrawWaterSettings();
	void DrawSHaRCSettings();
	void DrawDebugSettings();

	void UpdateSettings();
	bool Available() const;
	void ConvertTextures();

	/**
	 * @brief Builds the shared-buffer payload for FeatureData (b6).
	 *
	 * Mirrors Raytracing::GetCommonBufferData() in skyrim-community-shaders:
	 * vanilla ambient and env reflections are scaled to 0 while GI is
	 * available so CERT provides them instead of double-counting.
	 */
	struct alignas(16) SharedData
	{
		float Ambient = 1.0f;
		float Reflection = 1.0f;
		uint Albedo = 0;
		uint _padding0 = 0;
	};
	STATIC_ASSERT_ALIGNAS_16(SharedData);

	SharedData GetCommonBufferData() const;
};
