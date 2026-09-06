#include "Raytracing.h"

#include "Features/PathTracing.h"
#include "Features/Upscaling/DXVKInterop.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"

#define I18N_KEY_PREFIX "feature.raytracing."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Raytracing::Settings,
	Enabled,
	CreationEngineRaytracingSettings,
	RendererSettings)

void Raytracing::RestoreDefaultSettings()
{
	settings = {};
}

void Raytracing::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateSettings();
}

void Raytracing::SaveSettings(json& o_json)
{
	o_json = settings;
}

bool Raytracing::Available(bool a_initialized) const
{
	if (forcedDisabled || !loaded)
		return false;
	if (a_initialized && !initialized)
		return false;
	return settings.Enabled && settings.CreationEngineRaytracingSettings.Enabled;
}

CreationEngineRaytracing::Settings Raytracing::GetSettings() const
{
	auto certSettings = settings.CreationEngineRaytracingSettings;

	if (globals::features::pathTracing.loaded && globals::features::pathTracing.settings.Enabled) {
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::PathTracing;
		certSettings.GeneralSettings.Denoiser = globals::features::pathTracing.settings.GeneralSettings.Denoiser;
		certSettings.RaytracingSettings = globals::features::pathTracing.settings.RaytracingSettings;
	} else if (certSettings.GeneralSettings.Mode == CreationEngineRaytracing::Mode::PathTracing) {
		certSettings.GeneralSettings.Mode = CreationEngineRaytracing::Mode::None;
	}

	return certSettings;
}

CreationEngineRaytracing::Mode Raytracing::Mode() const
{
	if (!Available())
		return CreationEngineRaytracing::Mode::None;
	if (globals::features::pathTracing.loaded && globals::features::pathTracing.settings.Enabled)
		return CreationEngineRaytracing::Mode::PathTracing;
	return settings.CreationEngineRaytracingSettings.GeneralSettings.Mode;
}

bool Raytracing::IsPathTracing() const
{
	return Mode() == CreationEngineRaytracing::Mode::PathTracing;
}

void Raytracing::UpdateSettings()
{
	if (creationEngineRaytracing && creationEngineRaytracing->UpdateSettings) {
		creationEngineRaytracing->UpdateSettings(GetSettings());
	}
}

void Raytracing::Load()
{
	if (forcedDisabled)
		return;

	Hooks::Install();
}

void Raytracing::PostPostLoad()
{
	creationEngineRaytracing = std::make_unique<CreationEngineRaytracing>();

	if (!creationEngineRaytracing->handle) {
		settings.Enabled = false;
		settings.CreationEngineRaytracingSettings.Enabled = false;
		forcedDisabled = true;
		disableReason = DisableReason::MissingPlugin;
		logger::warn("[Raytracing] 'CreationEngineRaytracing.dll' not found, feature disabled.");
		return;
	}

	logger::info("[Raytracing] Loaded 'CreationEngineRaytracing.dll' module successfully.");
}

bool Raytracing::InitializeCERaytracing()
{
	if (forcedDisabled || initialized)
		return false;

	if (!creationEngineRaytracing || !creationEngineRaytracing->handle)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (dxvk && (dxvk->IsAvailable() || dxvk->Initialize())) {
		VkQueue graphicsQueue = dxvk->GetQueue();
		uint32_t queueIndex = 0;
		uint32_t queueFamilyIndex = dxvk->GetQueueFamilyIndex();
		dxvk->GetSubmissionQueue1(&graphicsQueue, &queueIndex, &queueFamilyIndex);

		if (creationEngineRaytracing->InitializeVulkanRenderer) {
			bool result = creationEngineRaytracing->InitializeVulkanRenderer(
				&settings.RendererSettings,
				dxvk->GetInstance(),
				dxvk->GetPhysicalDevice(),
				dxvk->GetDevice(),
				graphicsQueue,
				static_cast<int>(queueIndex),
				graphicsQueue,
				static_cast<int>(queueIndex),
				graphicsQueue,
				static_cast<int>(queueIndex));

			if (!result) {
				settings.Enabled = false;
				settings.CreationEngineRaytracingSettings.Enabled = false;
				initialized = false;
				forcedDisabled = true;
				disableReason = DisableReason::InitFailed;
				logger::error("[Raytracing] Failed to initialize Creation Engine ray tracing via Vulkan renderer.");
				return false;
			}

			initialized = true;
			UpdateResolution();
			logger::info("[Raytracing] Successfully initialized Creation Engine ray tracing (Vulkan).");
			return true;
		}
	}

	logger::warn("[Raytracing] Renderer backend unavailable or incompatible for Creation Engine ray tracing initialization.");
	return false;
}

bool Raytracing::UpdateResolution()
{
	if (!globals::game::graphicsState || !creationEngineRaytracing || !creationEngineRaytracing->SetResolution)
		return false;

	uint2 resolution{
		globals::game::graphicsState->screenWidth,
		globals::game::graphicsState->screenHeight
	};

	if (resolution == m_Resolution)
		return false;

	m_Resolution = resolution;
	creationEngineRaytracing->SetResolution(m_Resolution.x, m_Resolution.y);
	return true;
}

void Raytracing::SetupResources()
{
	if (forcedDisabled)
		return;

	if (!initialized) {
		InitializeCERaytracing();
	}

	if (initialized && creationEngineRaytracing) {
		if (creationEngineRaytracing->Initialize) {
			creationEngineRaytracing->Initialize(GetSettings());
		}
		UpdateResolution();
	}
}

void Raytracing::DrawSettings()
{
	if (disableReason == DisableReason::MissingPlugin) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_missing_plugin"), "Missing 'CreationEngineRaytracing.dll', check your mod manager."));
		return;
	} else if (disableReason == DisableReason::InitFailed) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("disable_reason_init_failed"), "Initialization Failed, check CreationEngineRaytracing.txt log"));
		return;
	}

	ImGui::Text("%s", T(TKEY("status_initialized"), "CreationEngineRaytracing runtime is active and initialized."));
	ImGui::Spacing();

	bool changed = false;
	if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.CreationEngineRaytracingSettings.Enabled)) {
		settings.Enabled = settings.CreationEngineRaytracingSettings.Enabled;
		changed = true;
	}

	const char* modeStr = "None";
	switch (Mode()) {
	case CreationEngineRaytracing::Mode::GlobalIllumination:
		modeStr = "Global Illumination";
		break;
	case CreationEngineRaytracing::Mode::PathTracing:
		modeStr = "Path Tracing";
		break;
	case CreationEngineRaytracing::Mode::Debug:
		modeStr = "Debug";
		break;
	default:
		break;
	}
	ImGui::Text("%s: %s", T(TKEY("active_mode"), "Active Mode"), modeStr);

	if (changed) {
		UpdateSettings();
	}
}
