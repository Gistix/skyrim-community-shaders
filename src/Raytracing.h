#pragma once

#include "CreationEngineRaytracing.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include "Globals.h"
#include <memory>

struct uint2
{
	uint32_t x = 0;
	uint32_t y = 0;

	bool operator==(const uint2&) const = default;
	bool operator!=(const uint2&) const = default;
};

/**
 * @brief Core feature integrating CreationEngineRaytracing hardware-accelerated ray tracing.
 */
struct Raytracing : Feature
{
public:
	// Metadata
	virtual std::string GetName() override { return "Raytracing"; }
	virtual std::string GetDisplayName() override { return T("feature.raytracing.name", "Raytracing"); }
	virtual std::string GetShortName() override { return "Raytracing"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }
	virtual bool DrawFailLoadMessage() const override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return std::make_pair(
			"Raytracing integrates hardware-accelerated ray tracing via Creation Engine Raytracing (CERT).",
			std::vector<std::string>{
				"Hardware ray tracing pipeline integration",
				"Support for Global Illumination and Path Tracing modes"
			});
	}

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Lifecycle
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	bool Available(bool a_initialized = true) const;
	bool InitializeCERaytracing();
	bool UpdateResolution();
	void UpdateSettings();
	CreationEngineRaytracing::Settings GetSettings() const;

	CreationEngineRaytracing::Mode Mode() const;
	bool IsPathTracing() const;

	enum struct DisableReason
	{
		None,
		UnsupportedGPU,
		OutdatedDrivers,
		MissingPlugin,
		InitFailed,
	};

	struct Settings
	{
		bool Enabled = true;
		CreationEngineRaytracing::Settings CreationEngineRaytracingSettings;
		CreationEngineRaytracing::RendererSettings RendererSettings;

		bool operator==(const Settings&) const = default;
	} settings;

	bool initialized = false;
	bool forcedDisabled = false;
	DisableReason disableReason = DisableReason::None;

	uint2 m_Resolution{ 0, 0 };

	std::unique_ptr<CreationEngineRaytracing> creationEngineRaytracing = nullptr;

	struct Hooks
	{
		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				auto& rt = globals::features::raytracing;
				if (rt.Available() && rt.creationEngineRaytracing && rt.Mode() != CreationEngineRaytracing::Mode::None) {
					if (rt.creationEngineRaytracing->UpdateCamera) {
						rt.creationEngineRaytracing->UpdateCamera();
					}
				}

				func(a1);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::detour_thunk<Main_RenderWorld>(REL::RelocationID(100424, 107142));
		}
	};
};