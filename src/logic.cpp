
#include "logic.h"

#include "vehicle_manager.h"
#include "vehicle_profile_manager.h"
#include "profile_storage.h"
#include "profile_sequence_runtime.h"

#include <mod/config.h>
#include <mod/logger.h>
#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>

namespace Logic {

static bool g_enabled;
static bool g_runningLED;
static bool g_strobo;
static bool g_rgbLighting;
static bool g_runningText;
static int g_brightness;
static float g_animationSpeed;

static VLS::VehicleManager g_vehicleManager;
static VLS::VehicleProfileManager g_profileManager;
static VLS::ProfileSequenceRuntime g_sequenceRuntime;

static const VLS::VehicleProfile* g_currentProfile = nullptr;
static int g_lastLoggedModelId = -2;
static bool g_sequenceSelectionAttempted = false;

static void ResetSequenceRuntime() {
    g_sequenceRuntime.Stop();
    g_sequenceSelectionAttempted = false;
}

static void UpdateSequenceRuntime() {
    if (g_currentProfile == nullptr) {
        return;
    }

    const uint32_t nowMs = CTimer::GetTimeMS();

    if (g_sequenceRuntime.IsRunning()) {
        g_sequenceRuntime.Update(
            *g_currentProfile,
            nowMs
        );

        return;
    }

    if (g_sequenceRuntime.IsFinished()) {
        return;
    }

    if (g_sequenceSelectionAttempted) {
        return;
    }

    g_sequenceSelectionAttempted = true;

    for (
        uint32_t i = 0;
        i < g_currentProfile->sequences.size();
        ++i
    ) {
        const VLS::EffectSequence& sequence =
            g_currentProfile->sequences[i];

        if (!sequence.enabled) {
            continue;
        }

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

    logger->Info(
        "No valid enabled sequence found for current vehicle."
    );
}

static void OnGameProcess() {
    if (!g_enabled) {
        return;
    }

    g_vehicleManager.Update();

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId != g_lastLoggedModelId) {
        g_lastLoggedModelId = modelId;

        ResetSequenceRuntime();

        if (modelId < 0) {
            g_currentProfile = nullptr;

            logger->Info(
                "Player is not using a valid vehicle."
            );

            return;
        }

        if (g_profileManager.Find(modelId) == nullptr) {
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

        const VLS::VehicleProfile* customProfile =
            g_profileManager.Find(modelId);

        const VLS::VehicleProfile& profile =
            g_profileManager.GetProfileOrDefault(modelId);

        g_currentProfile = &profile;

        if (customProfile != nullptr) {
            logger->Info(
                "Custom vehicle profile selected: Model ID %d, Placements: %d",
                modelId,
                static_cast<int>(profile.placements.size())
            );
        } else {
            logger->Info(
                "Default vehicle profile selected: Model ID %d, Placements: %d",
                modelId,
                static_cast<int>(profile.placements.size())
            );
        }
    }

    UpdateSequenceRuntime();
}

void Init() {
    logger->Info(
        "Initializing VLS configuration..."
    );

    g_enabled = cfg->GetBool(
        "Enabled",
        true,
        "General"
    );

    g_runningLED = cfg->GetBool(
        "RunningLED",
        true,
        "Effects"
    );

    g_strobo = cfg->GetBool(
        "Strobo",
        false,
        "Effects"
    );

    g_rgbLighting = cfg->GetBool(
        "RGBLighting",
        true,
        "Effects"
    );

    g_runningText = cfg->GetBool(
        "RunningText",
        false,
        "Effects"
    );

    g_brightness = cfg->GetInt(
        "Brightness",
        100,
        "Animation"
    );

    g_animationSpeed = cfg->GetFloat(
        "Speed",
        1.0f,
        "Animation"
    );

    if (g_brightness < 0) {
        g_brightness = 0;
    }

    if (g_brightness > 100) {
        g_brightness = 100;
    }

    if (g_animationSpeed < 0.1f) {
        g_animationSpeed = 0.1f;
    }

    if (g_animationSpeed > 10.0f) {
        g_animationSpeed = 10.0f;
    }

    if (VLS::ProfileStorage::Init()) {
        logger->Info(
            "VLS profile storage initialized: %s",
            VLS::ProfileStorage::GetProfilesDirectory()
        );
    } else {
        logger->Info(
            "VLS profile storage initialization failed."
        );
    }

    if (g_profileManager.Load(-1)) {
        logger->Info(
            "Default vehicle profile loaded."
        );
    } else {
        logger->Info(
            "No saved default profile found; using built-in defaults."
        );
    }

    if (g_enabled) {
        Events::gameProcessEvent += OnGameProcess;
    }

    logger->Info(
        "VLS configuration initialized."
    );
}

void Shutdown() {
    logger->Info(
        "Saving VLS profiles..."
    );

    ResetSequenceRuntime();

    if (g_profileManager.SaveAll()) {
        logger->Info(
            "VLS profiles saved successfully."
        );
    } else {
        logger->Info(
            "Some VLS profiles could not be saved."
        );
    }

    g_currentProfile = nullptr;
    g_lastLoggedModelId = -2;

    logger->Info(
        "VLS shutdown completed."
    );
}

const VLS::VehicleProfile* GetCurrentProfile() {
    return g_currentProfile;
}

} // namespace Logic
