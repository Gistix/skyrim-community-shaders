#include "Features/PathTracing.h"

#include "Globals.h"
#include "Menu.h"
#include "Raytracing.h"
#include "Upscaling.h"
#include "Upscaling/Streamline.h"

#define I18N_KEY_PREFIX "feature.path_tracing."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PathTracing::Settings,
	Enabled,
	RaytracingSettings,
	GeneralSettings)

void PathTracing::RestoreDefaultSettings()
{
	settings = {};
}

void PathTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateSettings();
}

void PathTracing::SaveSettings(json& o_json)
{
	o_json = settings;
}

bool PathTracing::Available() const
{
	return loaded && settings.Enabled && globals::features::raytracing.Available();
}

void PathTracing::UpdateSettings()
{
	globals::features::raytracing.UpdateSettings();
}

void PathTracing::DrawSettings()
{
	auto& rt = globals::features::raytracing;
	if (!rt.Available(false)) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("requires_raytracing"), "Creation Engine Raytracing runtime is not available. Ensure CreationEngineRaytracing.dll is installed."));
		return;
	}

	auto settingsBefore = settings;

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	ImGui::SliderInt(T(TKEY("bounces"), "Bounces"), &settings.RaytracingSettings.Bounces, 1, 8);

	ImGui::SliderInt(T(TKEY("samples_per_pixel"), "Samples Per Pixel"), &settings.RaytracingSettings.SamplesPerPixel, 1, 16);

	ImGui::SliderFloat(T(TKEY("resolution_scale"), "Resolution Scale"), &settings.RaytracingSettings.ResolutionScale, 0.25f, 1.0f, "%.2f");

	const char* rrNames[] = { "Disabled", "Standard", "Enhanced" };
	int currentRR = static_cast<int>(settings.RaytracingSettings.RussianRoulette);
	if (ImGui::Combo(T(TKEY("russian_roulette"), "Russian Roulette"), &currentRR, rrNames, IM_ARRAYSIZE(rrNames))) {
		settings.RaytracingSettings.RussianRoulette = static_cast<CreationEngineRaytracing::RussianRoulette>(currentRR);
	}

	const char* denoiserNames[] = { "None", "NRD Reblur", "NRD Relax", "DLSS RR", "Accumulation" };
	int currentDenoiser = static_cast<int>(settings.GeneralSettings.Denoiser);
	if (ImGui::Combo(T(TKEY("denoiser"), "Denoiser"), &currentDenoiser, denoiserNames, IM_ARRAYSIZE(denoiserNames))) {
		settings.GeneralSettings.Denoiser = static_cast<CreationEngineRaytracing::Denoiser>(currentDenoiser);
	}

	if (settings.GeneralSettings.Denoiser == CreationEngineRaytracing::Denoiser::DLSS_RR) {
		auto* streamline = Streamline::GetSingleton();
		if (!streamline->IsDLSSRRSupported()) {
			ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
				T("feature.raytracing.dlss_rr_not_available", "DLSS Ray Reconstruction is not available on this system."));
		} else if (globals::features::upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS_RR) {
			ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Warning, "%s",
				T("feature.raytracing.set_upscaling_to_dlss", "Set Upscaling method to DLSS to enable Ray Reconstruction."));
		}
	}

	if (settingsBefore != settings) {
		UpdateSettings();
	}
}
