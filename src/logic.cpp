
#include "logic.h"

#include "vehicle_manager.h"
#include "vehicle_profile_manager.h"
#include "profile_storage.h"
#include "profile_sequence_runtime.h"
#include "profile_renderer.h"
#include "vehicle_geometry_reader.h"
#include "vehicle_light_engine.h"
#include "animation_controller.h"
#include "vehicle_profile.h"

#include <mod/config.h>
#include <mod/logger.h>

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace Logic {

static bool g_enabled = true;
static bool g_editorOpen = false;
static bool g_imguiInitialized = false;

static int g_brightness = 100;
static float g_animationSpeed = 1.0f;

static int g_selectedPixel = 0;
static int g_selectedPlacement = 0;
static int g_selectedEffect = 0;
static uint32_t g_effectEditorPlacementId = 0;
static int g_editorPage = 0;
static int g_adjustmentScaleMode = 1;
static const char* g_adjustmentScaleNames[] = {"Very Fine", "Normal", "Fast", "Very Fast"};
static const float g_adjustmentScaleValues[] = {0.1f, 1.0f, 5.0f, 20.0f};
static char g_runningTextBuffer[129] = {};
static char g_runningFontBuffer[33] = {};
static char g_runningTextureBuffer[33] = {};
static const VLS::LedEffectConfig* g_runningTextBufferEffect = nullptr;
static const VLS::LedEffectConfig* g_runningFontBufferEffect = nullptr;
static const VLS::LedEffectConfig* g_runningTextureBufferEffect = nullptr;
static bool g_runningTextureCustomSelection = false;
static bool g_runningTextKeyboardOpen = false;

static VLS::VehicleManager g_vehicleManager;
static VLS::VehicleProfileManager g_profileManager;
static VLS::ProfileSequenceRuntime g_sequenceRuntime;
static VLS::AnimationController g_animationController; // Legacy controller.
static std::unordered_map<uint32_t, VLS::AnimationController> g_placementControllers;

static VLS::AnimationMode g_animationMode =
    VLS::AnimationMode::Manual;

static uint32_t g_animationIntervalMs = 3000;

static const VLS::VehicleProfile* g_currentProfile = nullptr;

static int g_lastLoggedModelId = -2;
static bool g_sequenceSelectionAttempted = false;

static std::unordered_set<int> g_profileLoadAttempted;
static std::unordered_set<int> g_pendingProfileLoads;

static std::vector<CVehicle*> g_frameVehicles;
static std::unordered_set<CVehicle*> g_frameVehicleSet;

static constexpr uint32_t MAX_MATRIX_PIXELS = 4096;


// ---------------------------------------------------------
// Vehicle geometry scan state
// ---------------------------------------------------------

static CVehicle* g_geometryScanVehicle = nullptr;
static int g_geometryScanModelId = -1;
static uint32_t g_nextGeometryScanTimeMs = 0;
static bool g_geometryScanSucceeded = false;
static bool g_geometryScanAttemptComplete = false;


// ---------------------------------------------------------
// Profile helpers
// ---------------------------------------------------------

static void ResetSequenceRuntime() {
    g_sequenceRuntime.Stop();
    g_sequenceSelectionAttempted = false;
}

static void ResetAnimationRuntime() {
    ResetSequenceRuntime();
    g_animationController.Reset();
    g_animationController.SetMode(g_animationMode);
    g_placementControllers.clear();
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

static VLS::VehicleProfile* EnsureEditorProfile(int modelId) {
    if (modelId < 0)
        return nullptr;

    VLS::VehicleProfile* profile =
        g_profileManager.Find(modelId);

    if (profile == nullptr)
        profile = &g_profileManager.GetOrCreate(modelId);

    if (profile->placements.empty()) {
        VLS::LedPlacement placement;

        placement.id = 1;
        placement.zone = VLS::LedZone::Windshield;

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

        effect.brightness = 1.0f;
        effect.speed = 1.0f;

        placement.effects.push_back(effect);
        profile->placements.push_back(placement);

        g_profileManager.MarkDirty(modelId);
    }

    VLS::LedPlacement& placement =
        profile->placements[0];

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
// Matrix helpers
// ---------------------------------------------------------

static bool SyncMatrixPixels(
    VLS::LedEffectConfig& effect
) {
    VLS::LedMatrixConfig& matrix = effect.matrix;

    if (matrix.columns == 0 || matrix.rows == 0)
        return false;

    uint32_t count =
        static_cast<uint32_t>(matrix.columns) *
        static_cast<uint32_t>(matrix.rows);

    if (count == 0 || count > MAX_MATRIX_PIXELS)
        return false;

    const size_t oldSize = matrix.pixels.size();

    if (oldSize == count)
        return true;

    matrix.pixels.resize(count);

    for (size_t i = oldSize; i < matrix.pixels.size(); ++i) {
        VLS::LedMatrixPixel& pixel = matrix.pixels[i];

        pixel.enabled = true;
        pixel.direction = VLS::LedDirection::LeftToRight;
        pixel.customAngle = 0.0f;

        pixel.red = effect.red;
        pixel.green = effect.green;
        pixel.blue = effect.blue;

        pixel.brightness = 1.0f;
        pixel.speed = 1.0f;
    }

    return true;
}


// ---------------------------------------------------------
// ImGui definitions
// ---------------------------------------------------------

static const char* g_patternNames[] = {
    "Running LED",
    "Running Text",
    "RGB",
    "Strobo Wave",
    "Strobo Eyes"
};

static const VLS::LedEffectType g_patternTypes[] = {
    VLS::LedEffectType::RunningLED,
    VLS::LedEffectType::RunningText,
    VLS::LedEffectType::RGB,
    VLS::LedEffectType::StroboWave,
    VLS::LedEffectType::StroboEyes
};

static const char* g_zoneNames[] = {
    "Front Windshield",
    "Exterior Body",
    "Headlight",
    "Taillight",
    "Interior",
    "Front Up Windshield",
    "Roof"
};

static const VLS::LedZone g_zoneTypes[] = {
    VLS::LedZone::Windshield,
    VLS::LedZone::ExteriorBody,
    VLS::LedZone::Headlight,
    VLS::LedZone::Taillight,
    VLS::LedZone::Interior,
    VLS::LedZone::FrontUpWindshield,
    VLS::LedZone::Roof
};

static const char* g_directionNames[] = {
    "Left to Right",
    "Right to Left",
    "Top to Bottom",
    "Bottom to Top",
    "Custom Angle"
};

static const VLS::LedDirection g_directionTypes[] = {
    VLS::LedDirection::LeftToRight,
    VLS::LedDirection::RightToLeft,
    VLS::LedDirection::TopToBottom,
    VLS::LedDirection::BottomToTop,
    VLS::LedDirection::CustomAngle
};

static const char* g_animationModeNames[] = {
    "Manual",
    "Automatic Motion",
    "Random Motion",
    "Sequential Motion",
    "Custom Motion (Reserved)"
};

static const VLS::AnimationMode g_animationModeTypes[] = {
    VLS::AnimationMode::Manual,
    VLS::AnimationMode::Automatic,
    VLS::AnimationMode::Random,
    VLS::AnimationMode::Sequential,
    VLS::AnimationMode::Custom
};

static int GetPatternIndex(VLS::LedEffectType type) {
    for (int i = 0; i < 5; ++i) {
        if (g_patternTypes[i] == type)
            return i;
    }

    return 0;
}

static int GetZoneIndex(VLS::LedZone zone) {
    for (int i = 0; i < 7; ++i) {
        if (g_zoneTypes[i] == zone)
            return i;
    }

    return 0;
}

static int GetDirectionIndex(VLS::LedDirection direction) {
    for (int i = 0; i < 5; ++i) {
        if (g_directionTypes[i] == direction)
            return i;
    }

    return 0;
}

static int GetAnimationModeIndex(VLS::AnimationMode mode) {
    for (int i = 0; i < 5; ++i) {
        if (g_animationModeTypes[i] == mode)
            return i;
    }

    return 0;
}


// ---------------------------------------------------------
// Animation controller helpers
// ---------------------------------------------------------

static void SetAnimationMode(VLS::AnimationMode mode) {
    if (g_animationMode == mode)
        return;

    g_animationMode = mode;

    g_animationController.SetMode(mode);
    g_animationController.SetIntervalMs(
        g_animationIntervalMs
    );

    ResetSequenceRuntime();

    logger->Info(
        "VLS animation mode changed: %d",
        static_cast<int>(mode)
    );
}

static void UpdateAnimationController() {
    if (g_currentProfile == nullptr)
        return;
    const uint32_t nowMs = CTimer::GetTimeMS();
    for (const VLS::LedPlacement& placement : g_currentProfile->placements) {
        auto inserted = g_placementControllers.emplace(placement.id, VLS::AnimationController());
        VLS::AnimationController& controller = inserted.first->second;
        if (inserted.second)
            controller.SetRandomSeed(static_cast<uint32_t>(g_currentProfile->modelId * 2654435761u) ^ (placement.id * 2246822519u));
        controller.SetMode(static_cast<VLS::AnimationMode>(placement.animationMode));
        controller.SetIntervalMs(placement.animationIntervalMs);
        controller.SetManualSelection(placement.id, placement.activeEffectIndex);
        controller.Update(placement, nowMs);
    }
}


// ---------------------------------------------------------
// ImGui launcher and editor
// ---------------------------------------------------------

static bool DrawSpacedFloatControl(
    const char* label, float* value, float minValue, float maxValue, float step
) {
    bool changed = false;
    const int scaleMode = g_adjustmentScaleMode < 0 ? 0 :
        (g_adjustmentScaleMode > 3 ? 3 : g_adjustmentScaleMode);
    step *= g_adjustmentScaleValues[scaleMode];
    ImGui::PushID(label);
    ImGui::Text("%s", label);
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(76.0f, 40.0f))) {
        *value -= step;
        if (*value < minValue) *value = minValue;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::Text("%.3f", *value);
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(76.0f, 40.0f))) {
        *value += step;
        if (*value > maxValue) *value = maxValue;
        changed = true;
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
    return changed;
}

static void DrawRunningTextKeyboard(VLS::LedEffectConfig& effect, bool& changed) {
    if (!g_runningTextKeyboardOpen)
        return;

    ImGui::Separator();
    ImGui::TextWrapped("VLS Touch Keyboard - QWERTY letters, numbers, spaces and editing.");
    const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", "1234567890"};
    const float keyWidth = 32.0f;
    for (const char* row : rows) {
        for (const char* key = row; *key; ++key) {
            char label[2] = {*key, '\0'};
            if (ImGui::Button(label, ImVec2(keyWidth, 38.0f))) {
                const size_t len = std::strlen(g_runningTextBuffer);
                if (len + 1 < sizeof(g_runningTextBuffer)) {
                    g_runningTextBuffer[len] = *key;
                    g_runningTextBuffer[len + 1] = '\0';
                    effect.text = g_runningTextBuffer;
                    changed = true;
                }
            }
            if (key[1]) ImGui::SameLine();
        }
    }

    if (ImGui::Button("SPACE", ImVec2(88.0f, 38.0f))) {
        const size_t len = std::strlen(g_runningTextBuffer);
        if (len + 1 < sizeof(g_runningTextBuffer)) {
            g_runningTextBuffer[len] = ' ';
            g_runningTextBuffer[len + 1] = '\0';
            effect.text = g_runningTextBuffer;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("BACKSPACE", ImVec2(104.0f, 38.0f))) {
        size_t len = std::strlen(g_runningTextBuffer);
        if (len > 0) {
            g_runningTextBuffer[len - 1] = '\0';
            effect.text = g_runningTextBuffer;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("CLEAR", ImVec2(70.0f, 38.0f))) {
        g_runningTextBuffer[0] = '\0';
        effect.text.clear();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("DONE", ImVec2(62.0f, 38.0f)))
        g_runningTextKeyboardOpen = false;
}

static void RenderEditor() {

    ImGui::SetNextWindowPos(
        ImVec2(20.0f, 100.0f),
        ImGuiCond_FirstUseEver
    );

    const ImGuiWindowFlags launcherFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin(
        "VLS Launcher",
        nullptr,
        launcherFlags
    );

    if (ImGui::Button(
            g_editorOpen ? "LED Editor: OPEN" : "LED",
            ImVec2(110.0f, 42.0f)
        )) {
        g_editorOpen = !g_editorOpen;
    }

    ImGui::End();

    if (!g_editorOpen)
        return;

    ImGui::SetNextWindowSize(
        ImVec2(460.0f, 580.0f),
        ImGuiCond_FirstUseEver
    );

    const ImGuiWindowFlags editorFlags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse;

    if (!ImGui::Begin(
            "Vehicle LED System",
            &g_editorOpen,
            editorFlags
        )) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 30.0f);
    ImGui::BeginChild("VLSContent", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::Text("Vehicle LED System");
    if (ImGui::Button("Scroll Up Fast", ImVec2(180.0f, 38.0f)))
        ImGui::SetScrollY(ImGui::GetScrollY() - 300.0f);
    ImGui::SameLine();
    if (ImGui::Button("Scroll Down Fast", ImVec2(180.0f, 38.0f)))
        ImGui::SetScrollY(ImGui::GetScrollY() + 300.0f);
    ImGui::Separator();

    ImGui::Checkbox(
        "LED System Enabled",
        &g_enabled
    );

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId < 0) {
        ImGui::Spacing();
        ImGui::Text(
            "Enter a vehicle to edit its profile."
        );

        ImGui::EndChild();
    ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    ImGui::Text(
        "Vehicle Model ID: %d",
        modelId
    );

    VLS::VehicleProfile* profile =
        EnsureEditorProfile(modelId);

    if (profile == nullptr) {
        ImGui::Text("Profile unavailable.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    ImGui::Separator();
    ImGui::Text("Native Vehicle Lighting");
    bool nativeLightsChanged = false;
    nativeLightsChanged |= ImGui::Checkbox("High Beam", &profile->nativeLights.highBeamEnabled);
    if (profile->nativeLights.highBeamEnabled) {
        nativeLightsChanged |= ImGui::SliderFloat("High Beam Range", &profile->nativeLights.highBeamRange, 10.0f, 500.0f);
        nativeLightsChanged |= ImGui::SliderFloat("High Beam Intensity", &profile->nativeLights.highBeamIntensity, 0.1f, 4.0f);
        ImGui::TextWrapped("Adds forward illumination while keeping the game's original low beam behavior.");
    }
    nativeLightsChanged |= ImGui::Checkbox("RGB Headlights", &profile->nativeLights.rgbHeadlightsEnabled);
    nativeLightsChanged |= ImGui::Checkbox("RGB Taillights", &profile->nativeLights.rgbTaillightsEnabled);
    if (profile->nativeLights.rgbHeadlightsEnabled || profile->nativeLights.rgbTaillightsEnabled) {
        nativeLightsChanged |= ImGui::SliderFloat("Native Light RGB Flow Speed", &profile->nativeLights.rgbFlowSpeed, 0.1f, 4.0f);
        nativeLightsChanged |= ImGui::SliderFloat("Native Light RGB Intensity", &profile->nativeLights.rgbFlowIntensity, 0.1f, 2.0f);
        ImGui::TextWrapped("Overlays RGB on detected lamp geometry, independent of vehiclelightson128 alpha. Only this player vehicle is affected.");
    }
    if (nativeLightsChanged)
        g_profileManager.MarkDirty(modelId);

    if (profile->placements.empty()) {
        ImGui::Text("No LED placements yet. Add an LED placement to configure strips.");
        ImGui::EndChild();
    ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    if (g_selectedPlacement < 0)
        g_selectedPlacement = 0;
    if (g_selectedPlacement >= static_cast<int>(profile->placements.size()))
        g_selectedPlacement = static_cast<int>(profile->placements.size()) - 1;

    if (g_editorPage == 0) {
    auto nextPlacementId = [&]() {
        uint32_t nextId = 1;
        for (const VLS::LedPlacement& existing : profile->placements)
            if (existing.id >= nextId) nextId = existing.id + 1;
        return nextId;
    };

    if (ImGui::Button("Add LED", ImVec2(112.0f, 40.0f))) {
        VLS::LedPlacement added;
        added.id = nextPlacementId();
        added.zone = VLS::LedZone::Windshield;
        added.transform.width = 1.0f;
        added.transform.height = 0.12f;
        VLS::LedEffectConfig newEffect;
        newEffect.type = VLS::LedEffectType::RunningLED;
        newEffect.enabled = true;
        newEffect.red = 0;
        newEffect.green = 180;
        newEffect.blue = 255;
        newEffect.brightness = 1.0f;
        newEffect.speed = 1.0f;
        added.effects.push_back(newEffect);
        profile->placements.push_back(added);
        g_selectedPlacement = static_cast<int>(profile->placements.size()) - 1;
        g_profileManager.MarkDirty(modelId);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy LED", ImVec2(112.0f, 40.0f))) {
        VLS::LedPlacement copied = profile->placements[g_selectedPlacement];
        copied.id = nextPlacementId();
        profile->placements.push_back(copied);
        g_selectedPlacement = static_cast<int>(profile->placements.size()) - 1;
        g_profileManager.MarkDirty(modelId);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete LED", ImVec2(112.0f, 40.0f))) {
        if (profile->placements.size() > 1) {
            profile->placements.erase(profile->placements.begin() + g_selectedPlacement);
            if (g_selectedPlacement >= static_cast<int>(profile->placements.size()))
                g_selectedPlacement = static_cast<int>(profile->placements.size()) - 1;
            g_profileManager.MarkDirty(modelId);
        }
    }
    ImGui::Text("LED placements: %d (copy duplicates position + effects)", static_cast<int>(profile->placements.size()));
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    if (profile->placements.size() > 1) {
        if (ImGui::Button("<< LED", ImVec2(90.0f, 32.0f))) {
            if (g_selectedPlacement > 0) --g_selectedPlacement;
        }
        ImGui::SameLine();
        ImGui::Text("Placement %d / %d", g_selectedPlacement + 1,
            static_cast<int>(profile->placements.size()));
        ImGui::SameLine();
        if (ImGui::Button("LED >>", ImVec2(90.0f, 32.0f))) {
            if (g_selectedPlacement + 1 < static_cast<int>(profile->placements.size()))
                ++g_selectedPlacement;
        }
    }

    VLS::LedPlacement& placement =
        profile->placements[g_selectedPlacement];

    if (ImGui::Checkbox("LED Placement Always On", &placement.enabled))
        g_profileManager.MarkDirty(modelId);

    bool changed = false;

    if (g_effectEditorPlacementId != placement.id) {
        g_effectEditorPlacementId = placement.id;
        g_selectedEffect = static_cast<int>(placement.activeEffectIndex);
    }
    if (g_selectedEffect < 0) g_selectedEffect = 0;
    if (g_selectedEffect >= static_cast<int>(placement.effects.size()))
        g_selectedEffect = placement.effects.empty() ? 0 : static_cast<int>(placement.effects.size()) - 1;

    if (ImGui::Button("Add Effect", ImVec2(125.0f, 38.0f))) {
        VLS::LedEffectConfig addedEffect;
        addedEffect.type = VLS::LedEffectType::RunningLED;
        addedEffect.red = 0; addedEffect.green = 180; addedEffect.blue = 255;
        placement.effects.push_back(addedEffect);
        g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Effect", ImVec2(125.0f, 38.0f)) && !placement.effects.empty()) {
        placement.effects.push_back(placement.effects[g_selectedEffect]);
        g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Effect", ImVec2(125.0f, 38.0f)) && placement.effects.size() > 1) {
        placement.effects.erase(placement.effects.begin() + g_selectedEffect);
        if (g_selectedEffect >= static_cast<int>(placement.effects.size()))
            g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }

    if (placement.effects.empty()) {
        ImGui::TextWrapped("Add an effect to this LED placement to begin.");
        ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::End();
        return;
    }
    ImGui::Text("Effects on this LED: %d", static_cast<int>(placement.effects.size()));
    if (ImGui::SliderInt("Selected Effect", &g_selectedEffect, 0, static_cast<int>(placement.effects.size()) - 1)) {
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }
    if (ImGui::Button("Move Effect Up", ImVec2(145.0f, 34.0f)) && g_selectedEffect > 0) {
        std::swap(placement.effects[g_selectedEffect], placement.effects[g_selectedEffect - 1]);
        --g_selectedEffect;
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Move Effect Down", ImVec2(145.0f, 34.0f)) &&
        g_selectedEffect + 1 < static_cast<int>(placement.effects.size())) {
        std::swap(placement.effects[g_selectedEffect], placement.effects[g_selectedEffect + 1]);
        ++g_selectedEffect;
        placement.activeEffectIndex = static_cast<uint32_t>(g_selectedEffect);
        changed = true;
    }
    VLS::LedEffectConfig& effect = placement.effects[g_selectedEffect];

    if (ImGui::Button("LED & EFFECT", ImVec2(190.0f, 36.0f)))
        g_editorPage = 0;
    ImGui::SameLine();
    if (ImGui::Button("POSITION", ImVec2(190.0f, 36.0f)))
        g_editorPage = 1;
    ImGui::Separator();

    if (g_editorPage == 0) {
    // -----------------------------------------------------
    // General effects use the placement-level motion controller.
    if (effect.type != VLS::LedEffectType::StroboEyes &&
        effect.type != VLS::LedEffectType::RunningText) {
    ImGui::Separator();
    ImGui::Text("Motion Pattern Controller - This LED Only");
    int modeIndex = static_cast<int>(placement.animationMode);
    if (ImGui::Combo("Motion Mode", &modeIndex, g_animationModeNames, 5)) {
        if (modeIndex < 0) modeIndex = 0;
        if (modeIndex > 4) modeIndex = 4;
        placement.animationMode = static_cast<uint8_t>(modeIndex);
        g_placementControllers[placement.id].Reset();
        changed = true;
    }
    int interval = static_cast<int>(placement.animationIntervalMs);
    if (ImGui::SliderInt("Motion Pattern Interval (ms)", &interval, 500, 30000)) {
        placement.animationIntervalMs = static_cast<uint32_t>(interval);
        g_placementControllers[placement.id].Reset();
        changed = true;
    }
    if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Manual))
        ImGui::TextWrapped("Manual: keeps the selected Motion Pattern and selected LED effect.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Automatic))
        ImGui::TextWrapped("Automatic: cycles a curated Motion Pattern sequence. The selected LED effect never changes.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Random))
        ImGui::TextWrapped("Random: chooses a new Motion Pattern at each interval. The selected LED effect never changes.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Sequential))
        ImGui::TextWrapped("Sequential: steps through every Motion Pattern in order. The selected LED effect never changes.");
    else {
        ImGui::TextWrapped("Custom Motion Sequence will be added in a later stage. The selected LED effect remains fixed.");
    }
    } else if (effect.type == VLS::LedEffectType::StroboEyes) {
        ImGui::TextWrapped("Strobo Eyes uses its dedicated pupil movement, expression playback, eye visuals, and eyebrow controls.");
    } else {
        ImGui::TextWrapped("Running Text uses its dedicated direction, bounce, font, and texture controls.");
    }

        // LED Zone
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Zone");

    int zoneIndex = GetZoneIndex(placement.zone);

    if (ImGui::Combo(
            "Placement Zone",
            &zoneIndex,
            g_zoneNames,
            7
        )) {
        placement.zone = g_zoneTypes[zoneIndex];
        changed = true;
    }

    }

    if (g_editorPage == 1) {
    // -----------------------------------------------------
    // Placement
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Placement");

    ImGui::Text("Position adjustment speed");
    ImGui::Combo("Adjustment Speed", &g_adjustmentScaleMode, g_adjustmentScaleNames, 4);
    ImGui::Text("Very Fine = 0.1x | Normal = 1x | Fast = 5x | Very Fast = 20x");
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    changed |= DrawSpacedFloatControl("Position X", &placement.transform.x, -10.0f, 10.0f, 0.01f);
    changed |= DrawSpacedFloatControl("Position Y", &placement.transform.y, -10.0f, 10.0f, 0.01f);
    changed |= DrawSpacedFloatControl("Position Z", &placement.transform.z, -10.0f, 10.0f, 0.01f);

    ImGui::Text("Size");
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    changed |= DrawSpacedFloatControl("Width", &placement.transform.width, 0.1f, 6.0f, 0.01f);
    changed |= DrawSpacedFloatControl("Height", &placement.transform.height, 0.02f, 1.0f, 0.005f);

    ImGui::Separator();
    ImGui::Text("Orientation & Stretch");
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    changed |= DrawSpacedFloatControl("Rotation X", &placement.transform.rotationX, -180.0f, 180.0f, 0.5f);
    changed |= DrawSpacedFloatControl("Rotation Y", &placement.transform.rotationY, -180.0f, 180.0f, 0.5f);
    changed |= DrawSpacedFloatControl("Rotation Z", &placement.transform.rotationZ, -180.0f, 180.0f, 0.5f);
    ImGui::Separator();
    ImGui::Text("Curvature / Bend");
    changed |= DrawSpacedFloatControl("Curve Depth", &placement.transform.curvature, -1.5f, 1.5f, 0.01f);
    ImGui::Text("Facing Direction / POV");
    changed |= DrawSpacedFloatControl("POV Yaw", &placement.transform.facingYaw, -90.0f, 90.0f, 0.5f);
    changed |= DrawSpacedFloatControl("POV Pitch", &placement.transform.facingPitch, -90.0f, 90.0f, 0.5f);
    changed |= DrawSpacedFloatControl("Stretch X", &placement.transform.stretchX, 0.1f, 4.0f, 0.01f);
    changed |= DrawSpacedFloatControl("Stretch Y", &placement.transform.stretchY, 0.1f, 4.0f, 0.01f);

    }

    if (g_editorPage == 0) {
    // -----------------------------------------------------
    // Effect
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Effect");

    changed |= ImGui::Checkbox(
        "Effect Enabled",
        &effect.enabled
    );

    int patternIndex =
        GetPatternIndex(effect.type);

    if (ImGui::Combo(
            "Pattern",
            &patternIndex,
            g_patternNames,
            5
        )) {
        effect.type = g_patternTypes[patternIndex];
        if (effect.type == VLS::LedEffectType::RunningText ||
            effect.type == VLS::LedEffectType::StroboEyes) {
            effect.matrix.enabled = true;
            if (effect.matrix.rows < 7)
                effect.matrix.rows = 7;
            if (effect.matrix.columns < 24)
                effect.matrix.columns = 24;
            SyncMatrixPixels(effect);
        }
        changed = true;
    }

    if (effect.type == VLS::LedEffectType::RunningText) {
        if (g_runningTextBufferEffect != &effect) {
            std::memset(g_runningTextBuffer, 0, sizeof(g_runningTextBuffer));
            std::strncpy(g_runningTextBuffer, effect.text.c_str(), sizeof(g_runningTextBuffer) - 1);
            g_runningTextBufferEffect = &effect;
        }
        ImGui::TextWrapped("Current message: %s", g_runningTextBuffer[0] ? g_runningTextBuffer : "(empty)");
        if (ImGui::Button(g_runningTextKeyboardOpen ? "Hide Touch Keyboard" : "Open Touch Keyboard", ImVec2(230.0f, 44.0f)))
            g_runningTextKeyboardOpen = !g_runningTextKeyboardOpen;
        DrawRunningTextKeyboard(effect, changed);
        changed |= ImGui::SliderFloat("Text Spacing", &effect.textSpacing, 0.25f, 3.0f);
        changed |= ImGui::SliderFloat("Font Size", &effect.textScale, 0.35f, 2.0f);
        ImGui::TextWrapped("For clear small text, keep Font Size around 0.65 or higher. Very small glyphs merge because each LED is one physical pixel; increase Matrix Columns for finer detail.");
        if (g_runningFontBufferEffect != &effect) {
            std::memset(g_runningFontBuffer, 0, sizeof(g_runningFontBuffer));
            std::strncpy(g_runningFontBuffer, effect.fontName.c_str(), sizeof(g_runningFontBuffer) - 1);
            g_runningFontBufferEffect = &effect;
        }
        static const char* fontChoices[] = {"default", "Custom..."};
        int fontChoice = effect.fontName == "default" ? 0 : 1;
        if (ImGui::Combo("Bitmap Font", &fontChoice, fontChoices, IM_ARRAYSIZE(fontChoices))) {
            if (fontChoice == 0) effect.fontName = "default";
            else if (effect.fontName == "default") effect.fontName.clear();
            g_runningFontBufferEffect = nullptr;
            changed = true;
        }
        if (fontChoice == 1) {
            if (g_runningFontBufferEffect != &effect) {
                std::memset(g_runningFontBuffer, 0, sizeof(g_runningFontBuffer));
                std::strncpy(g_runningFontBuffer, effect.fontName.c_str(), sizeof(g_runningFontBuffer) - 1);
                g_runningFontBufferEffect = &effect;
            }
            if (ImGui::InputText("Custom Font Name", g_runningFontBuffer, sizeof(g_runningFontBuffer))) {
                effect.fontName = g_runningFontBuffer;
                changed = true;
            }
        }
        ImGui::TextWrapped("Bundled choice: default.vlsfont. Custom files belong in VehicleLEDSystem/fonts/.");

        if (g_runningTextureBufferEffect != &effect) {
            std::memset(g_runningTextureBuffer, 0, sizeof(g_runningTextureBuffer));
            std::strncpy(g_runningTextureBuffer, effect.textureName.c_str(), sizeof(g_runningTextureBuffer) - 1);
            g_runningTextureCustomSelection =
                !effect.textureName.empty() && effect.textureName != "demo";
            g_runningTextureBufferEffect = &effect;
        }
        static const char* textureChoices[] = {"(No Texture)", "demo", "Custom..."};
        int textureChoice = g_runningTextureCustomSelection ? 2 :
            (effect.textureName.empty() ? 0 : (effect.textureName == "demo" ? 1 : 2));
        if (ImGui::Combo("Matrix Texture", &textureChoice, textureChoices, IM_ARRAYSIZE(textureChoices))) {
            if (textureChoice == 0) {
                effect.textureName.clear();
                g_runningTextureCustomSelection = false;
            } else if (textureChoice == 1) {
                effect.textureName = "demo";
                g_runningTextureCustomSelection = false;
            } else {
                effect.textureName.clear();
                g_runningTextureCustomSelection = true;
            }
            g_runningTextureBufferEffect = nullptr;
            changed = true;
        }
        if (g_runningTextureCustomSelection) {
            if (g_runningTextureBufferEffect != &effect) {
                std::memset(g_runningTextureBuffer, 0, sizeof(g_runningTextureBuffer));
                std::strncpy(g_runningTextureBuffer, effect.textureName.c_str(), sizeof(g_runningTextureBuffer) - 1);
                g_runningTextureBufferEffect = &effect;
            }
            if (ImGui::InputText("Custom Texture Name", g_runningTextureBuffer, sizeof(g_runningTextureBuffer))) {
                effect.textureName = g_runningTextureBuffer;
                changed = true;
            }
        }
        ImGui::TextWrapped("Bundled choice: demo.ppm. Custom P3 PPM files belong in VehicleLEDSystem/textures/.");
        static const char* textMotionNames[] = {"Scroll Left", "Scroll Right", "Bounce Horizontal", "Scroll Up", "Scroll Down", "Bounce Vertical", "Static / Center"};
        int textMotionIndex = static_cast<int>(effect.textMotion);
        if (ImGui::Combo("Text Motion", &textMotionIndex, textMotionNames, 7)) {
            if (textMotionIndex < 0) textMotionIndex = 0;
            if (textMotionIndex > 6) textMotionIndex = 6;
            effect.textMotion = static_cast<VLS::RunningTextMotion>(textMotionIndex);
            changed = true;
        }
        ImGui::TextWrapped("Choose Pattern > Running Text, open the touch keyboard, type your message, then tap DONE and Save Profile.");
        static const char* textLaserNames[] = {"Off", "Laser Write", "Laser Weld"};
        int textLaserIndex = effect.motionPattern == VLS::LedMotionPattern::LaserWrite ? 1 :
            (effect.motionPattern == VLS::LedMotionPattern::LaserWeld ? 2 : 0);
        if (ImGui::Combo("Text Laser Effect", &textLaserIndex, textLaserNames, 3)) {
            effect.motionPattern = textLaserIndex == 1
                ? VLS::LedMotionPattern::LaserWrite
                : (textLaserIndex == 2 ? VLS::LedMotionPattern::LaserWeld
                                       : VLS::LedMotionPattern::LeftToRight);
            changed = true;
        }
        ImGui::TextWrapped("Laser Write reveals the moving text with a bright beam; Laser Weld adds a hotter head and a fading spark trail.");
    }

    if (effect.type == VLS::LedEffectType::RunningText ||
        effect.type == VLS::LedEffectType::StroboEyes) {
        if (!effect.matrix.enabled) {
            effect.matrix.enabled = true;
            changed = true;
        }
        if (effect.matrix.rows < 7) {
            effect.matrix.rows = 7;
            changed = true;
        }
        if (effect.type == VLS::LedEffectType::StroboEyes &&
            effect.eyeBrowStyle != VLS::EyeBrowStyle::None &&
            effect.matrix.rows < 10) {
            effect.matrix.rows = 10;
            changed = true;
        }
        if (effect.matrix.columns < 24) {
            effect.matrix.columns = 24;
            changed = true;
        }
        const size_t expectedPixels =
            static_cast<size_t>(effect.matrix.columns) * effect.matrix.rows;
        if (effect.matrix.pixels.size() != expectedPixels) {
            SyncMatrixPixels(effect);
            changed = true;
        }
    }

    if (effect.type != VLS::LedEffectType::StroboEyes &&
        effect.type != VLS::LedEffectType::RunningText) {
        static const char* motionNames[] = {
            "Left to Right", "Right to Left", "Center Out", "Edges to Center",
            "Zoom In", "Zoom Out", "Fade In", "Fade Out", "Rolling",
            "Flow", "Wave", "Diagonal", "Ripple", "Pulse",
            "Alternate Rows", "Alternate Columns", "Checkerboard",
            "Windmill", "Spiral", "Starburst", "Laser Write", "Laser Weld",
            "Arrow Left", "Arrow Right", "Arrow Up", "Arrow Down", "Opposing Arrows",
            "Center Collision", "Center Burst", "Diagonal + Collision", "Diagonal + Center Burst",
            "Multi Collision", "Cross Burst", "Radial Burst", "Ping-Pong",
            "Mirror Reflection", "Reflective Wave", "Collision Rebound", "Center Split",
            "Dual Flow", "Mirrored Diagonal", "Double Rebound",
            "Vertical Sweep", "Side Collision", "Corner Burst",
            "Twin Spiral", "Orbit Ring"
        };
        int motionIndex = static_cast<int>(effect.motionPattern);
        if (ImGui::Combo("Motion Pattern", &motionIndex, motionNames, 47)) {
            if (motionIndex < 0) motionIndex = 0;
            if (motionIndex > 46) motionIndex = 46;
            effect.motionPattern = static_cast<VLS::LedMotionPattern>(motionIndex);
            changed = true;
        }
        ImGui::TextWrapped("Collision / Burst: Multi Collision, Cross Burst and Radial Burst add intersecting or expanding fronts. Rebound patterns bounce after impact; Reflection patterns reverse or mirror their wave direction.");
        ImGui::TextWrapped("Laser Write draws a moving beam with a fading trace. Laser Weld adds a hot core and spark-like trail. Color Style controls the beam palette.");
    } else if (effect.type == VLS::LedEffectType::StroboEyes) {
        ImGui::Separator();
        ImGui::Text("STROBO EYES - Dedicated Eye Engine");
        static const char* eyeShapeNames[] = {"Classic", "Narrow", "Wide", "Round", "Cat Eye", "Square", "Sleep"};
        int eyeShape = static_cast<int>(effect.eyeShape);
        if (ImGui::Combo("Eye Shape", &eyeShape, eyeShapeNames, 7)) {
            effect.eyeShape = static_cast<VLS::EyeShape>(eyeShape); changed = true;
        }
        static const char* playbackNames[] = {"Manual", "Automatic", "Sequence", "Random"};
        static const char* eyeMotionNames[] = {"Idle", "Look Left", "Look Right", "Look Up", "Look Down", "Scan", "Wink", "Shake", "Rapid Blink", "Sharingan", "Mangekyou", "Rinnegan"};
        int eyeMotion = static_cast<int>(effect.eyeMotion);
        if (ImGui::Combo("Eye Motion", &eyeMotion, eyeMotionNames, 12)) {
            effect.eyeMotion = static_cast<VLS::EyeMotion>(eyeMotion); changed = true;
        }
        int eyeMotionPlayback = static_cast<int>(effect.eyeMotionPlayback);
        if (ImGui::Combo("Pupil Motion Playback", &eyeMotionPlayback, playbackNames, 4)) {
            effect.eyeMotionPlayback = static_cast<VLS::MotionPlaybackMode>(eyeMotionPlayback); changed = true;
        }
        if (effect.eyeMotionPlayback != VLS::MotionPlaybackMode::Manual) {
            int interval = static_cast<int>(effect.eyeMotionIntervalMs);
            if (ImGui::SliderInt("Pupil Motion Interval (ms)", &interval, 500, 15000)) {
                effect.eyeMotionIntervalMs = static_cast<uint32_t>(interval); changed = true;
            }
        }
        int blinkInterval = static_cast<int>(effect.eyeBlinkIntervalMs);
        if (ImGui::SliderInt("Blink Interval (ms)", &blinkInterval, 500, 8000)) {
            effect.eyeBlinkIntervalMs = static_cast<uint32_t>(blinkInterval); changed = true;
        }
        float irisColor[3] = {effect.eyeIrisRed / 255.0f, effect.eyeIrisGreen / 255.0f, effect.eyeIrisBlue / 255.0f};
        if (ImGui::ColorEdit3("Iris Color", irisColor)) {
            effect.eyeIrisRed = static_cast<uint8_t>(irisColor[0] * 255.0f);
            effect.eyeIrisGreen = static_cast<uint8_t>(irisColor[1] * 255.0f);
            effect.eyeIrisBlue = static_cast<uint8_t>(irisColor[2] * 255.0f); changed = true;
        }
        float pupilColor[3] = {effect.eyePupilRed / 255.0f, effect.eyePupilGreen / 255.0f, effect.eyePupilBlue / 255.0f};
        if (ImGui::ColorEdit3("Pupil Color", pupilColor)) {
            effect.eyePupilRed = static_cast<uint8_t>(pupilColor[0] * 255.0f);
            effect.eyePupilGreen = static_cast<uint8_t>(pupilColor[1] * 255.0f);
            effect.eyePupilBlue = static_cast<uint8_t>(pupilColor[2] * 255.0f); changed = true;
        }
        static const char* eyeBrowNames[] = {"Off", "Natural", "Sharp", "Thick"};
        int eyeBrow = static_cast<int>(effect.eyeBrowStyle);
        if (ImGui::Combo("Eyebrows", &eyeBrow, eyeBrowNames, 4)) {
            effect.eyeBrowStyle = static_cast<VLS::EyeBrowStyle>(eyeBrow); changed = true;
        }
        if (effect.eyeBrowStyle != VLS::EyeBrowStyle::None) {
            float browColor[3] = {effect.eyeBrowRed / 255.0f, effect.eyeBrowGreen / 255.0f, effect.eyeBrowBlue / 255.0f};
            if (ImGui::ColorEdit3("Eyebrow Color", browColor)) {
                effect.eyeBrowRed = static_cast<uint8_t>(browColor[0] * 255.0f);
                effect.eyeBrowGreen = static_cast<uint8_t>(browColor[1] * 255.0f);
                effect.eyeBrowBlue = static_cast<uint8_t>(browColor[2] * 255.0f); changed = true;
            }
            if (ImGui::SliderFloat("Eyebrow Tilt", &effect.eyeBrowTilt, -1.0f, 1.0f)) changed = true;
        }
        ImGui::TextWrapped("Eye Visual selects a persistent iris motif. Selecting a motif disables Automatic Eye Transformation so the chosen Sharingan, Mangekyou, or Rinnegan remains active. Blink can briefly close the eye; expressions and gaze no longer replace the iris design.");
    } else if (effect.type == VLS::LedEffectType::RunningText) {
        ImGui::TextWrapped("Text scrolling, transitions and text patterns use dedicated text controls, separate from general LED motion patterns.");
    }

    if (effect.matrix.enabled &&
        ImGui::Checkbox("Transparent Matrix Background", &effect.transparentMatrixBackground)) {
        changed = true;
    }
    ImGui::TextWrapped("When enabled, dim/off matrix cells are skipped so the black inner background becomes transparent.");

    float opacity = effect.opacity;
    if (ImGui::SliderFloat("LED Transparency (0=hidden, 1=solid)", &opacity, 0.0f, 1.0f)) {
        effect.opacity = opacity;
        changed = true;
    }

    const bool supportsPalette =
        effect.type == VLS::LedEffectType::RunningLED ||
        effect.type == VLS::LedEffectType::RunningText ||
        effect.type == VLS::LedEffectType::StroboWave ||
        effect.type == VLS::LedEffectType::StroboEyes;
    if (supportsPalette) {
        const char* colorModeNames[] = {
            "Solid Color", "Two-Color Gradient", "Rainbow",
            "Three-Color Gradient", "Color Cycle", "Color Wave",
            "Color Pulse", "Alternating Colors"
        };
        int colorMode = static_cast<int>(effect.colorMode);
        if (ImGui::Combo("Color Style", &colorMode, colorModeNames, 8)) {
            effect.colorMode = static_cast<VLS::LedColorMode>(colorMode);
            changed = true;
        }

        if (effect.colorMode == VLS::LedColorMode::Gradient ||
            effect.colorMode == VLS::LedColorMode::ThreeColorGradient ||
            effect.colorMode == VLS::LedColorMode::ColorPulse ||
            effect.colorMode == VLS::LedColorMode::Alternating) {
            float secondaryColor[3] = {
                effect.secondaryRed / 255.0f,
                effect.secondaryGreen / 255.0f,
                effect.secondaryBlue / 255.0f
            };
            if (ImGui::ColorEdit3("Secondary Color", secondaryColor)) {
                effect.secondaryRed = static_cast<uint8_t>(secondaryColor[0] * 255.0f);
                effect.secondaryGreen = static_cast<uint8_t>(secondaryColor[1] * 255.0f);
                effect.secondaryBlue = static_cast<uint8_t>(secondaryColor[2] * 255.0f);
                changed = true;
            }
        }
        if (effect.colorMode == VLS::LedColorMode::ThreeColorGradient) {
            float tertiaryColor[3] = {
                effect.tertiaryRed / 255.0f,
                effect.tertiaryGreen / 255.0f,
                effect.tertiaryBlue / 255.0f
            };
            if (ImGui::ColorEdit3("Third Gradient Color", tertiaryColor)) {
                effect.tertiaryRed = static_cast<uint8_t>(tertiaryColor[0] * 255.0f);
                effect.tertiaryGreen = static_cast<uint8_t>(tertiaryColor[1] * 255.0f);
                effect.tertiaryBlue = static_cast<uint8_t>(tertiaryColor[2] * 255.0f);
                changed = true;
            }
        }
        if (effect.colorMode != VLS::LedColorMode::Solid &&
            effect.colorMode != VLS::LedColorMode::ColorCycle &&
            effect.colorMode != VLS::LedColorMode::ColorPulse) {
            const char* colorDirectionNames[] = {
                "Left to Right", "Right to Left", "Top to Bottom", "Bottom to Top",
                "Diagonal Down", "Diagonal Up", "Center Out", "Edges to Center"
            };
            int direction = static_cast<int>(effect.colorDirection);
            if (ImGui::Combo("Color Direction", &direction, colorDirectionNames, 8)) {
                effect.colorDirection = static_cast<VLS::LedColorDirection>(direction);
                changed = true;
            }
            float spread = effect.colorSpread;
            if (ImGui::SliderFloat("Color Spread", &spread, 0.25f, 4.0f)) {
                effect.colorSpread = spread;
                changed = true;
            }
        }
        ImGui::TextWrapped("Color direction and spread are independent from the LED motion pattern. RGB keeps its own animated spectrum.");
    } else if (effect.type == VLS::LedEffectType::RGB) {
        const char* colorDirectionNames[] = {
            "Left to Right", "Right to Left", "Top to Bottom", "Bottom to Top",
            "Diagonal Down", "Diagonal Up", "Center Out", "Edges to Center"
        };
        int direction = static_cast<int>(effect.colorDirection);
        if (ImGui::Combo("RGB Color Direction", &direction, colorDirectionNames, 8)) {
            effect.colorDirection = static_cast<VLS::LedColorDirection>(direction);
            changed = true;
        }
        float spread = effect.colorSpread;
        if (ImGui::SliderFloat("RGB Color Spread", &spread, 0.25f, 4.0f)) {
            effect.colorSpread = spread;
            changed = true;
        }
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

    float effectColor[3] = {
        effect.red / 255.0f,
        effect.green / 255.0f,
        effect.blue / 255.0f
    };
    if (ImGui::ColorEdit3("Effect Color", effectColor)) {
        effect.red = static_cast<uint8_t>(effectColor[0] * 255.0f);
        effect.green = static_cast<uint8_t>(effectColor[1] * 255.0f);
        effect.blue = static_cast<uint8_t>(effectColor[2] * 255.0f);
        changed = true;
    }

    // -----------------------------------------------------
    // Matrix Configuration
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Matrix");

    changed |= ImGui::Checkbox(
        "Matrix Enabled",
        &effect.matrix.enabled
    );

    if (effect.matrix.enabled) {

        int columns = effect.matrix.columns;
        int rows = effect.matrix.rows;

        bool dimensionsChanged = false;

        if (ImGui::SliderInt(
                "Matrix Columns",
                &columns,
                1,
                64
            )) {
            dimensionsChanged = true;
        }

        if (ImGui::SliderInt(
                "Matrix Rows",
                &rows,
                1,
                64
            )) {
            dimensionsChanged = true;
        }

        if (dimensionsChanged) {
            if (columns < 1) columns = 1;
            if (rows < 1) rows = 1;

            if (columns * rows >
                static_cast<int>(MAX_MATRIX_PIXELS)) {
                rows =
                    static_cast<int>(MAX_MATRIX_PIXELS) /
                    columns;

                if (rows < 1)
                    rows = 1;
            }

            effect.matrix.columns =
                static_cast<uint16_t>(columns);

            effect.matrix.rows =
                static_cast<uint16_t>(rows);

            if (SyncMatrixPixels(effect)) {
                g_selectedPixel = 0;
                changed = true;
            }
        }

        if (SyncMatrixPixels(effect)) {

            const int pixelCount =
                static_cast<int>(effect.matrix.pixels.size());

            ImGui::Text(
                "Total Matrix Pixels: %d",
                pixelCount
            );

            if (g_selectedPixel >= pixelCount)
                g_selectedPixel = pixelCount - 1;

            if (g_selectedPixel < 0)
                g_selectedPixel = 0;

            ImGui::Separator();
            ImGui::Text("Individual Pixel Editor");

            ImGui::SliderInt(
                "Selected Pixel",
                &g_selectedPixel,
                0,
                pixelCount - 1
            );

            const int pixelColumn =
                g_selectedPixel %
                effect.matrix.columns;

            const int pixelRow =
                g_selectedPixel /
                effect.matrix.columns;

            ImGui::Text(
                "Pixel: %d | Column: %d | Row: %d",
                g_selectedPixel,
                pixelColumn,
                pixelRow
            );

            VLS::LedMatrixPixel& pixel =
                effect.matrix.pixels[g_selectedPixel];

            changed |= ImGui::Checkbox(
                "Pixel Enabled",
                &pixel.enabled
            );

            int directionIndex =
                GetDirectionIndex(pixel.direction);

            if (ImGui::Combo(
                    "Pixel Direction",
                    &directionIndex,
                    g_directionNames,
                    5
                )) {
                pixel.direction =
                    g_directionTypes[directionIndex];

                changed = true;
            }

            if (pixel.direction ==
                VLS::LedDirection::CustomAngle) {

                changed |= ImGui::SliderFloat(
                    "Custom Angle",
                    &pixel.customAngle,
                    0.0f,
                    360.0f
                );
            }

            float pixelColor[3] = {
                pixel.red / 255.0f,
                pixel.green / 255.0f,
                pixel.blue / 255.0f
            };

            if (ImGui::ColorEdit3(
                    "Pixel Color",
                    pixelColor
                )) {
                pixel.red = static_cast<uint8_t>(
                    pixelColor[0] * 255.0f
                );

                pixel.green = static_cast<uint8_t>(
                    pixelColor[1] * 255.0f
                );

                pixel.blue = static_cast<uint8_t>(
                    pixelColor[2] * 255.0f
                );

                changed = true;
            }

            changed |= ImGui::SliderFloat(
                "Pixel Brightness",
                &pixel.brightness,
                0.0f,
                1.0f
            );

            changed |= ImGui::SliderFloat(
                "Pixel Speed",
                &pixel.speed,
                0.1f,
                10.0f
            );

        } else {
            ImGui::Text(
                "Invalid matrix configuration."
            );
        }
    }

    }

    // -----------------------------------------------------
    // Dirty state
    // -----------------------------------------------------

    if (changed)
        g_profileManager.MarkDirty(modelId);

    // -----------------------------------------------------
    // Save
    // -----------------------------------------------------

    ImGui::Separator();

    if (ImGui::Button(
            "Save Profile",
            ImVec2(160.0f, 40.0f)
        )) {
        g_profileManager.MarkDirty(modelId);

        if (g_profileManager.Save(modelId)) {
            logger->Info(
                "Profile saved: Model ID %d",
                modelId
            );
        } else {
            logger->Info(
                "Profile save failed: Model ID %d",
                modelId
            );
        }
    }

    ImGui::SameLine();

    if (g_editorPage == 1) {
        if (ImGui::Button(
                "Reset Position",
                ImVec2(160.0f, 40.0f)
            )) {
            placement.transform.x = 0.0f;
            placement.transform.y = 0.0f;
            placement.transform.z = 0.0f;
            placement.transform.rotationX = 0.0f;
            placement.transform.rotationY = 0.0f;
            placement.transform.rotationZ = 0.0f;
            placement.transform.curvature = 0.0f;
            placement.transform.facingYaw = 0.0f;
            placement.transform.facingPitch = 0.0f;
            placement.transform.stretchX = 1.0f;
            placement.transform.stretchY = 1.0f;
            g_profileManager.MarkDirty(modelId);
        }
    }

    ImGui::Spacing();

    ImGui::Text(
        "Changes are applied immediately."
    );

    ImGui::Text(
        "Close this window without disabling LED."
    );

    ImGui::EndChild();
    ImGui::PopStyleVar();
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

    // Register the player's high-beam point lights before the vehicle render
    // pass. RenderEffects runs after the main scene and is too late for lights
    // that must participate in the current frame's world lighting.
    if (vehicle == g_vehicleManager.GetCurrentVehicle()) {
        const VLS::VehicleProfile* profile = g_profileManager.Find(modelId);
        if (profile != nullptr && profile->enabled &&
            profile->nativeLights.highBeamEnabled) {
            VLS::VehicleLightEngine::AddHighBeam(vehicle, *profile);
        }
    }

    if (g_profileManager.Find(modelId) == nullptr &&
        g_profileLoadAttempted.find(modelId) ==
            g_profileLoadAttempted.end()) {
        g_pendingProfileLoads.insert(modelId);
    }

    if (g_frameVehicleSet.insert(vehicle).second)
        g_frameVehicles.push_back(vehicle);
}

static void OnRenderEffects() {
    if (!g_enabled) {
        g_frameVehicles.clear(); g_frameVehicleSet.clear(); return;
    }
    CVehicle* currentVehicle = g_vehicleManager.GetCurrentVehicle();
    if (currentVehicle == nullptr || !IsVehiclePointerValid(currentVehicle)) {
        g_frameVehicles.clear(); g_frameVehicleSet.clear(); return;
    }
    const int modelId = g_vehicleManager.GetCurrentModelId();
    if (modelId >= 0) {
        const VLS::VehicleProfile* profile = g_profileManager.Find(modelId);
        if (profile != nullptr && profile->enabled) {
            if (g_geometryScanSucceeded &&
                g_geometryScanVehicle == currentVehicle &&
                g_geometryScanModelId == modelId &&
                (profile->nativeLights.rgbHeadlightsEnabled ||
                 profile->nativeLights.rgbTaillightsEnabled)) {
                VLS::ProfileRenderer::RenderNativeLights(currentVehicle, *profile);
            }
            for (CVehicle* vehicle : g_frameVehicles) {
                if (vehicle != currentVehicle || !IsVehiclePointerValid(vehicle)) continue;
                for (const VLS::LedPlacement& led : profile->placements) {
                    if (!led.enabled || led.effects.empty()) continue;
                    const uint32_t selected = led.activeEffectIndex < led.effects.size()
                        ? led.activeEffectIndex : 0u;
                    VLS::ProfileRenderer::RenderSelectedEffect(
                        vehicle, *profile, led.id, selected
                    );
                }
            }
        }
    }
    g_frameVehicles.clear(); g_frameVehicleSet.clear();
}


// Vehicle geometry scan
// ---------------------------------------------------------

static void ResetVehicleGeometryScan() {
    // Lepaskan mesh kendaraan sebelumnya.
    VLS::VehicleGeometryReader::Clear();

    g_geometryScanVehicle = nullptr;
    g_geometryScanModelId = -1;
    g_nextGeometryScanTimeMs = 0;
    g_geometryScanSucceeded = false;
    g_geometryScanAttemptComplete = false;
}

static void UpdateVehicleGeometryScan() {
    CVehicle* vehicle = g_vehicleManager.GetCurrentVehicle();
    const int modelId = g_vehicleManager.GetCurrentModelId();

    if (vehicle == nullptr ||
        !IsVehiclePointerValid(vehicle) ||
        modelId < 0) {
        ResetVehicleGeometryScan();
        return;
    }

    if (vehicle != g_geometryScanVehicle ||
        modelId != g_geometryScanModelId) {
        VLS::VehicleGeometryReader::Clear();
        g_geometryScanVehicle = vehicle;
        g_geometryScanModelId = modelId;
        g_geometryScanSucceeded = false;
        g_geometryScanAttemptComplete = false;
        g_nextGeometryScanTimeMs = 0;
    }

    if (g_geometryScanAttemptComplete)
        return;

    // A model can be selected before its RenderWare clump is ready.
    // Retry that short readiness check periodically, but advance the
    // actual geometry pipeline every game frame once the clump exists.
    if (vehicle->m_pRwClump == nullptr) {
        const uint32_t nowMs = CTimer::GetTimeMS();
        if (static_cast<int32_t>(
                nowMs - g_nextGeometryScanTimeMs
            ) < 0)
            return;
        g_nextGeometryScanTimeMs = nowMs + 500;
    }

    VLS::GeometryScanStats stats;
    bool complete = false;
    const bool succeeded =
        VLS::VehicleGeometryReader::ScanIncremental(
            vehicle,
            modelId,
            stats,
            complete
        );

    if (complete) {
        g_geometryScanAttemptComplete = true;
        g_geometryScanSucceeded = succeeded;
    }
}


// ---------------------------------------------------------
// Sequence runtime
// ---------------------------------------------------------

static void UpdateSequenceRuntime() {
    if (g_currentProfile == nullptr)
        return;

    if (g_animationMode != VLS::AnimationMode::Custom) {
        if (g_sequenceRuntime.IsRunning())
            g_sequenceRuntime.Stop();

        g_sequenceSelectionAttempted = false;
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
                "Vehicle custom sequence started: Index %u",
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

        ResetAnimationRuntime();

        if (modelId < 0) {
            g_currentProfile = nullptr;
            ResetVehicleGeometryScan();

            logger->Info(
                "Player is not using a valid vehicle."
            );

            return;
        }

        EnsureProfileLoaded(modelId);
    }

    if (modelId < 0) {
        g_currentProfile = nullptr;
        ResetVehicleGeometryScan();
        return;
    }

    const VLS::VehicleProfile& profile =
        g_profileManager.GetProfileOrDefault(modelId);

    g_currentProfile = &profile;

    const bool nativeRgbRequested =
        g_enabled &&
        profile.enabled &&
        (profile.nativeLights.rgbHeadlightsEnabled ||
         profile.nativeLights.rgbTaillightsEnabled);

    if (nativeRgbRequested)
        UpdateVehicleGeometryScan();
    else
        ResetVehicleGeometryScan();

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

    g_animationController.SetMode(g_animationMode);
    g_animationController.SetIntervalMs(
        g_animationIntervalMs
    );

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
    Events::renderEffectsEvent += OnRenderEffects;

    logger->Info(
        "VLS callbacks registered."
    );
}


// ---------------------------------------------------------
// Shutdown
// ---------------------------------------------------------

void Shutdown() {
    logger->Info(
        "Saving Vehicle LED System profiles..."
    );

    ResetAnimationRuntime();
    ResetVehicleGeometryScan();

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
