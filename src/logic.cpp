
#include "logic.h"

#include "vehicle_manager.h"
#include "vehicle_profile_manager.h"
#include "profile_storage.h"
#include "profile_sequence_runtime.h"
#include "profile_renderer.h"
#include "vehicle_profile.h"

#include <mod/config.h>
#include <mod/logger.h>

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

#include <unordered_set>
#include <vector>
#include <cstdint>

namespace Logic {

static bool g_enabled = true;
static bool g_imguiInitialized = false;

static int g_brightness = 100;
static float g_animationSpeed = 1.0f;

static VLS::VehicleManager g_vehicleManager;
static VLS::VehicleProfileManager g_profileManager;
static VLS::ProfileSequenceRuntime g_sequenceRuntime;

static const VLS::VehicleProfile* g_currentProfile = nullptr;

static int g_lastLoggedModelId = -2;
static bool g_sequenceSelectionAttempted = false;

static std::unordered_set<int> g_profileLoadAttempted;
static std::unordered_set<int> g_pendingProfileLoads;

static std::vector<CVehicle*> g_frameVehicles;
static std::unordered_set<CVehicle*> g_frameVehicleSet;


// ---------------------------------------------------------
// Profile helpers
// ---------------------------------------------------------

static void ResetSequenceRuntime() {
    g_sequenceRuntime.Stop();
    g_sequenceSelectionAttempted = false;
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
            "No saved profile for model ID %d.",
            modelId
        );
    }
}

static void ProcessPendingProfileLoads() {
    for (const int modelId : g_pendingProfileLoads)
        EnsureProfileLoaded(modelId);

    g_pendingProfileLoads.clear();
}

// Membuat profil dan placement awal jika belum tersedia.
// Posisi awal X, Y, Z semuanya 0.
static VLS::VehicleProfile* EnsureEditorProfile(int modelId) {
    if (modelId < 0)
        return nullptr;

    VLS::VehicleProfile* profile =
        g_profileManager.Find(modelId);

    if (profile == nullptr) {
        profile = &g_profileManager.GetOrCreate(modelId);
    }

    if (profile->placements.empty()) {
        VLS::LedPlacement placement;

        placement.id = 1;
        placement.zone = VLS::LedZone::ExteriorBody;

        placement.transform.x = 0.0f;
        placement.transform.y = 0.0f;
        placement.transform.z = 0.0f;

        placement.transform.rotationX = 0.0f;
        placement.transform.rotationY = 0.0f;
        placement.transform.rotationZ = 0.0f;

        placement.transform.width = 2.5f;
        placement.transform.height = 0.18f;

        VLS::LedEffectConfig effect;
        effect.type = VLS::LedEffectType::RunningLED;
        effect.enabled = true;

        effect.red = 0;
        effect.green = 180;
        effect.blue = 255;

        effect.brightness =
            static_cast<float>(g_brightness) / 100.0f;

        effect.speed = g_animationSpeed;

        placement.effects.push_back(effect);
        profile->placements.push_back(placement);

        g_profileManager.MarkDirty(modelId);
    }

    // Pastikan placement pertama memiliki minimal satu efek.
    VLS::LedPlacement& placement = profile->placements[0];

    if (placement.effects.empty()) {
        VLS::LedEffectConfig effect;

        effect.type = VLS::LedEffectType::RunningLED;
        effect.enabled = true;
        effect.red = 0;
        effect.green = 180;
        effect.blue = 255;
        effect.brightness = 1.0f;
        effect.speed = 1.0f;

        placement.effects.push_back(effect);
        g_profileManager.MarkDirty(modelId);
    }

    return profile;
}


// ---------------------------------------------------------
// ImGui editor
// ---------------------------------------------------------

static const char* g_patternNames[] = {
    "Running LED",
    "RGB",
    "Strobo Wave",
    "Strobo Eyes"
};

static const VLS::LedEffectType g_patternTypes[] = {
    VLS::LedEffectType::RunningLED,
    VLS::LedEffectType::RGB,
    VLS::LedEffectType::StroboWave,
    VLS::LedEffectType::StroboEyes
};

static int GetPatternIndex(VLS::LedEffectType type) {
    for (int i = 0; i < 4; ++i) {
        if (g_patternTypes[i] == type)
            return i;
    }

    return 0;
}

static void RenderEditor() {
    ImGui::SetNextWindowSize(
        ImVec2(480.0f, 600.0f),
        ImGuiCond_FirstUseEver
    );

    ImGui::Begin("Vehicle LED System");

    ImGui::Text("Vehicle LED System - Editor");
    ImGui::Separator();

    ImGui::Checkbox("LED System Enabled", &g_enabled);

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId < 0) {
        ImGui::Spacing();
        ImGui::Text("Enter a vehicle to edit its profile.");
        ImGui::End();
        return;
    }

    ImGui::Text("Vehicle Model ID: %d", modelId);

    VLS::VehicleProfile* profile =
        EnsureEditorProfile(modelId);

    if (profile == nullptr ||
        profile->placements.empty()) {
        ImGui::Text("Profile is not available.");
        ImGui::End();
        return;
    }

    VLS::LedPlacement& placement =
        profile->placements[0];

    if (placement.effects.empty()) {
        ImGui::Text("No LED effect available.");
        ImGui::End();
        return;
    }

    VLS::LedEffectConfig& effect =
        placement.effects[0];

    bool changed = false;

    ImGui::Separator();
    ImGui::Text("LED Placement");

    changed |= ImGui::SliderFloat(
        "Position X",
        &placement.transform.x,
        -3.0f,
        3.0f
    );

    changed |= ImGui::SliderFloat(
        "Position Y",
        &placement.transform.y,
        -3.0f,
        3.0f
    );

    changed |= ImGui::SliderFloat(
        "Position Z",
        &placement.transform.z,
        -1.0f,
        3.0f
    );

    changed |= ImGui::SliderFloat(
        "Width",
        &placement.transform.width,
        0.1f,
        6.0f
    );

    changed |= ImGui::SliderFloat(
        "Height",
        &placement.transform.height,
        0.02f,
        1.0f
    );

    ImGui::Separator();
    ImGui::Text("Animation");

    changed |= ImGui::Checkbox(
        "Animation ON",
        &effect.enabled
    );

    int patternIndex = GetPatternIndex(effect.type);

    if (ImGui::Combo(
            "Pattern",
            &patternIndex,
            g_patternNames,
            4
        )) {
        effect.type = g_patternTypes[patternIndex];
        changed = true;
    }

    float brightness = effect.brightness;

    if (ImGui::SliderFloat(
            "Brightness",
            &brightness,
            0.0f,
            1.0f
        )) {
        effect.brightness = brightness;
        changed = true;
    }

    float speed = effect.speed;

    if (ImGui::SliderFloat(
            "Speed",
            &speed,
            0.1f,
            10.0f
        )) {
        effect.speed = speed;
        changed = true;
    }

    if (changed)
        g_profileManager.MarkDirty(modelId);

    ImGui::Separator();

    if (ImGui::Button("Save Profile")) {
        g_profileManager.MarkDirty(modelId);

        if (g_profileManager.Save(modelId)) {
            logger->Info(
                "Profile saved for vehicle model %d.",
                modelId
            );
        } else {
            logger->Info(
                "Failed to save profile for vehicle model %d.",
                modelId
            );
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Reset Position")) {
        placement.transform.x = 0.0f;
        placement.transform.y = 0.0f;
        placement.transform.z = 0.0f;

        g_profileManager.MarkDirty(modelId);
    }

    ImGui::Separator();

    ImGui::Text(
        "Changes are applied immediately."
    );

    ImGui::Text(
        "Save Profile stores settings for this vehicle model."
    );

    ImGui::End();
}


// ---------------------------------------------------------
// Vehicle rendering
// ---------------------------------------------------------

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

    CVehicle* currentVehicle =
        g_vehicleManager.GetCurrentVehicle();

    if (currentVehicle == nullptr ||
        !IsVehiclePointerValid(currentVehicle)) {
        g_frameVehicles.clear();
        g_frameVehicleSet.clear();
        return;
    }

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId >= 0) {
        const VLS::VehicleProfile* profile =
            g_profileManager.Find(modelId);

        if (profile != nullptr && profile->enabled) {
            for (CVehicle* vehicle : g_frameVehicles) {
                if (vehicle != currentVehicle)
                    continue;

                if (!IsVehiclePointerValid(vehicle))
                    continue;

                VLS::ProfileRenderer::RenderVehicleProfile(
                    vehicle,
                    *profile
                );
            }
        }
    }

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();
}


// ---------------------------------------------------------
// Sequence runtime
// ---------------------------------------------------------

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


// ---------------------------------------------------------
// Game process
// ---------------------------------------------------------

static void OnGameProcess() {
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

            logger->Info(
                "Player is not using a valid vehicle."
            );

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


// ---------------------------------------------------------
// Initialization
// ---------------------------------------------------------

void Init() {
    logger->Info(
        "Initializing Vehicle LED System..."
    );

    g_enabled = cfg->GetBool(
        "Enabled",
        true,
        "General"
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

    if (g_brightness < 0)
        g_brightness = 0;

    if (g_brightness > 100)
        g_brightness = 100;

    if (g_animationSpeed < 0.1f)
        g_animationSpeed = 0.1f;

    if (g_animationSpeed > 10.0f)
        g_animationSpeed = 10.0f;

    if (!VLS::ProfileStorage::Init()) {
        logger->Info(
            "VLS profile storage initialization failed."
        );
    } else {
        logger->Info(
            "VLS profile storage initialized: %s",
            VLS::ProfileStorage::GetProfilesDirectory()
        );
    }

    if (g_profileManager.Load(-1)) {
        logger->Info(
            "Default vehicle profile loaded."
        );
    }

    if (!ImGui_ImplRenderWare_Init(RenderEditor)) {
        logger->Info(
            "VLS ImGui initialization failed."
        );
    } else {
        g_imguiInitialized = true;

        logger->Info(
            "VLS ImGui editor initialized."
        );
    }

    Events::gameProcessEvent += OnGameProcess;
    Events::vehicleRenderEvent += OnVehicleRender;
    Events::frameEndEvent += OnFrameEnd;

    logger->Info(
        "VLS render callbacks registered."
    );

    logger->Info(
        "Vehicle LED System initialization completed."
    );
}


// ---------------------------------------------------------
// Shutdown
// ---------------------------------------------------------

void Shutdown() {
    logger->Info(
        "Saving Vehicle LED System profiles..."
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

    if (g_imguiInitialized) {
        ImGui_ImplRenderWare_Shutdown();
        g_imguiInitialized = false;
    }

    g_currentProfile = nullptr;

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();

    g_pendingProfileLoads.clear();
    g_profileLoadAttempted.clear();

    g_lastLoggedModelId = -2;

    logger->Info(
        "Vehicle LED System shutdown completed."
    );
}

const VLS::VehicleProfile* GetCurrentProfile() {
    return g_currentProfile;
}

} // namespace Logic
