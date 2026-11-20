
#include "logic.h"

#include "vehicle_manager.h"
#include "vehicle_profile_manager.h"
#include "profile_storage.h"
#include "profile_sequence_runtime.h"
#include "profile_renderer.h"

#include <mod/config.h>
#include <mod/logger.h>

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>

#include <unordered_set>
#include <vector>

namespace Logic {

static bool g_enabled = true;
static bool g_runningLED = true;
static bool g_strobo = false;
static bool g_rgbLighting = true;
static bool g_runningText = false;

static int g_brightness = 100;
static float g_animationSpeed = 1.0f;

static VLS::VehicleManager g_vehicleManager;
static VLS::VehicleProfileManager g_profileManager;
static VLS::ProfileSequenceRuntime g_sequenceRuntime;

static VLS::VehicleProfile g_testProfile;
static const VLS::VehicleProfile* g_currentProfile = nullptr;

static int g_lastLoggedModelId = -2;
static bool g_sequenceSelectionAttempted = false;

static std::unordered_set<int> g_profileLoadAttempted;
static std::unordered_set<int> g_pendingProfileLoads;

static std::vector<CVehicle*> g_frameVehicles;
static std::unordered_set<CVehicle*> g_frameVehicleSet;

static void ResetSequenceRuntime() {
    g_sequenceRuntime.Stop();
    g_sequenceSelectionAttempted = false;
}

static void BuildTestProfile() {
    g_testProfile.modelId = -1;
    g_testProfile.enabled = true;
    g_testProfile.placements.clear();

    VLS::LedPlacement placement;
    placement.id = 1;
    placement.zone = VLS::LedZone::ExteriorBody;

    // Koordinat awal perkiraan untuk pengujian.
    // Sumbu Z diarahkan ke bagian depan kendaraan.
    placement.transform.x = 0.0f;
    placement.transform.y = -0.35f;
    placement.transform.z = 1.65f;
    placement.transform.width = 1.6f;
    placement.transform.height = 0.10f;

    const float brightness =
        static_cast<float>(g_brightness) / 100.0f;

    if (g_runningLED) {
        VLS::LedEffectConfig effect;
        effect.type = VLS::LedEffectType::RunningLED;
        effect.enabled = true;
        effect.red = 0;
        effect.green = 180;
        effect.blue = 255;
        effect.brightness = brightness;
        effect.speed = g_animationSpeed;

        placement.effects.push_back(effect);
    }

    if (g_strobo) {
        VLS::LedEffectConfig effect;
        effect.type = VLS::LedEffectType::StroboWave;
        effect.enabled = true;
        effect.red = 255;
        effect.green = 255;
        effect.blue = 255;
        effect.brightness = brightness;
        effect.speed = g_animationSpeed;

        placement.effects.push_back(effect);
    }

    if (g_rgbLighting) {
        VLS::LedEffectConfig effect;
        effect.type = VLS::LedEffectType::RGB;
        effect.enabled = true;
        effect.red = 255;
        effect.green = 255;
        effect.blue = 255;
        effect.brightness = brightness;
        effect.speed = g_animationSpeed;

        placement.effects.push_back(effect);
    }

    if (!placement.effects.empty())
        g_testProfile.placements.push_back(placement);

    logger->Info(
        "VLS test profile prepared: RunningLED=%d Strobo=%d RGB=%d",
        g_runningLED,
        g_strobo,
        g_rgbLighting
    );
}

static void EnsureProfileLoaded(int modelId) {
    if (modelId < 0)
        return;

    if (!g_profileLoadAttempted.insert(modelId).second)
        return;

    if (g_profileManager.Find(modelId) != nullptr)
        return;

    if (g_profileManager.Load(modelId)) {
        logger->Info(
            "Vehicle profile loaded: Model ID %d",
            modelId
        );
    } else {
        logger->Info(
            "No saved profile for model ID %d; using default profile.",
            modelId
        );
    }
}

static void ProcessPendingProfileLoads() {
    for (const int modelId : g_pendingProfileLoads)
        EnsureProfileLoaded(modelId);

    g_pendingProfileLoads.clear();
}

static void OnVehicleRender(CVehicle* vehicle) {
    if (!g_enabled || vehicle == nullptr)
        return;

    if (!IsVehiclePointerValid(vehicle))
        return;

    const int modelId = vehicle->m_nModelIndex;

    if (modelId < 0)
        return;

    if (g_profileManager.Find(modelId) == nullptr &&
        g_profileLoadAttempted.find(modelId) ==
            g_profileLoadAttempted.end()) {
        g_pendingProfileLoads.insert(modelId);
    }

    if (g_frameVehicleSet.insert(vehicle).second)
        g_frameVehicles.push_back(vehicle);
}

static void OnFrameEnd(bool fromIdle) {
    (void)fromIdle;

    if (!g_enabled) {
        g_frameVehicles.clear();
        g_frameVehicleSet.clear();
        return;
    }

    const int playerModelId =
        g_vehicleManager.GetCurrentModelId();

    for (CVehicle* vehicle : g_frameVehicles) {
        if (vehicle == nullptr ||
            !IsVehiclePointerValid(vehicle))
            continue;

        const int modelId = vehicle->m_nModelIndex;

        if (modelId < 0 || modelId != playerModelId)
            continue;

        // Mode pengujian: hanya render pada model kendaraan
        // yang sedang digunakan pemain.
        if (!g_testProfile.placements.empty()) {
            VLS::ProfileRenderer::RenderVehicleProfile(
                vehicle,
                g_testProfile
            );
        }
    }

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();
}

static void UpdateSequenceRuntime() {
    if (g_currentProfile == nullptr)
        return;

    const uint32_t nowMs = CTimer::GetTimeMS();

    if (g_sequenceRuntime.IsRunning()) {
        g_sequenceRuntime.Update(*g_currentProfile, nowMs);
        return;
    }

    if (g_sequenceRuntime.IsFinished() ||
        g_sequenceSelectionAttempted)
        return;

    g_sequenceSelectionAttempted = true;

    for (uint32_t i = 0;
         i < g_currentProfile->sequences.size();
         ++i) {
        const VLS::EffectSequence& sequence =
            g_currentProfile->sequences[i];

        if (!sequence.enabled)
            continue;

        if (g_sequenceRuntime.Start(
                *g_currentProfile,
                i,
                nowMs
            )) {
            logger->Info(
                "Vehicle sequence started: Index %u",
                static_cast<unsigned int>(i)
            );
            return;
        }
    }
}

static void OnGameProcess() {
    if (!g_enabled)
        return;

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();

    g_vehicleManager.Update();
    ProcessPendingProfileLoads();

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId != g_lastLoggedModelId) {
        g_lastLoggedModelId = modelId;
        ResetSequenceRuntime();

        if (modelId < 0) {
            g_currentProfile = nullptr;
            logger->Info("Player is not using a valid vehicle.");
            return;
        }

        EnsureProfileLoaded(modelId);
    }

    if (modelId < 0) {
        g_currentProfile = nullptr;
        return;
    }

    const VLS::VehicleProfile& profile =
        g_profileManager.GetProfileOrDefault(modelId);

    g_currentProfile = &profile;

    UpdateSequenceRuntime();
}

void Init() {
    logger->Info("Initializing VLS configuration...");

    g_enabled = cfg->GetBool("Enabled", true, "General");

    g_runningLED = cfg->GetBool(
        "RunningLED", true, "Effects"
    );

    g_rgbLighting = cfg->GetBool(
        "RGBLighting", true, "Effects"
    );

    g_strobo = cfg->GetBool(
        "Strobo", false, "Effects"
    );

    g_runningText = cfg->GetBool(
        "RunningText", false, "Effects"
    );

    g_brightness = cfg->GetInt(
        "Brightness", 100, "Animation"
    );

    g_animationSpeed = cfg->GetFloat(
        "Speed", 1.0f, "Animation"
    );

    if (g_brightness < 0)
        g_brightness = 0;

    if (g_brightness > 100)
        g_brightness = 100;

    if (g_animationSpeed < 0.1f)
        g_animationSpeed = 0.1f;

    if (g_animationSpeed > 10.0f)
        g_animationSpeed = 10.0f;

    if (!VLS::ProfileStorage::Init()) {
        logger->Info("VLS profile storage initialization failed.");
    } else {
        logger->Info(
            "VLS profile storage initialized: %s",
            VLS::ProfileStorage::GetProfilesDirectory()
        );
    }

    if (g_profileManager.Load(-1)) {
        logger->Info("Default vehicle profile loaded.");
    } else {
        logger->Info(
            "No saved default profile found; using built-in defaults."
        );
    }

    BuildTestProfile();

    if (g_runningText) {
        logger->Info(
            "RunningText is enabled in config but not implemented yet."
        );
    }

    if (g_enabled) {
        Events::gameProcessEvent += OnGameProcess;
        Events::vehicleRenderEvent += OnVehicleRender;
        Events::frameEndEvent += OnFrameEnd;

        logger->Info("VLS render callbacks registered.");
    }

    logger->Info("VLS initialization completed.");
}

void Shutdown() {
    logger->Info("Saving VLS profiles...");

    ResetSequenceRuntime();

    if (g_profileManager.SaveAll()) {
        logger->Info("VLS profiles saved successfully.");
    } else {
        logger->Info("Some VLS profiles could not be saved.");
    }

    g_currentProfile = nullptr;

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();
    g_pendingProfileLoads.clear();
    g_profileLoadAttempted.clear();

    g_testProfile.placements.clear();

    g_lastLoggedModelId = -2;

    logger->Info("VLS shutdown completed.");
}

const VLS::VehicleProfile* GetCurrentProfile() {
    return g_currentProfile;
}

} // namespace Logic
