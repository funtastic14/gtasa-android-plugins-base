
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
#include <cstdio>
#include <string>

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
static char g_eyeTextureBuffer[33] = {};
static const VLS::LedEffectConfig* g_eyeTextureBufferEffect = nullptr;
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
static VLS::NativeLightScanMode g_geometryScanMode =
    VLS::NativeLightScanMode::ModelId;
static bool g_geometryScanSucceeded = false;
static bool g_geometryScanAttemptComplete = false;
static bool g_geometryScanClumpUnavailable = false;
static uintptr_t g_geometryScanClumpIdentity = 0;
static bool g_geometryScanDiagnosticLogged = false;
static bool g_nativeRgbRenderDiagnosticLogged = false;


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

static bool g_touchKeyboardUppercase = false;

static void DrawTouchKeyboard(
    char* buffer,
    size_t capacity,
    std::string& destination,
    bool& changed
) {
    if (!buffer || capacity == 0)
        return;

    ImGui::Separator();
    ImGui::TextWrapped("VLS Virtual Keyboard - tap keys to enter text.");
    const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm", "1234567890"};
    const float keyWidth = 32.0f;
    for (const char* row : rows) {
        for (const char* key = row; *key; ++key) {
            char label[2] = {
                static_cast<char>(g_touchKeyboardUppercase
                    ? (*key >= 'a' && *key <= 'z' ? *key - 'a' + 'A' : *key)
                    : *key),
                '\0'
            };
            if (ImGui::Button(label, ImVec2(keyWidth, 38.0f))) {
                const size_t len = std::strlen(buffer);
                if (len + 1 < capacity) {
                    buffer[len] = label[0];
                    buffer[len + 1] = '\0';
                    destination = buffer;
                    changed = true;
                }
            }
            if (key[1]) ImGui::SameLine();
        }
    }

    if (ImGui::Button(g_touchKeyboardUppercase ? "ABC" : "abc", ImVec2(62.0f, 38.0f)))
        g_touchKeyboardUppercase = !g_touchKeyboardUppercase;
    ImGui::SameLine();
    if (ImGui::Button("_", ImVec2(38.0f, 38.0f))) {
        const size_t len = std::strlen(buffer);
        if (len + 1 < capacity) {
            buffer[len] = '_';
            buffer[len + 1] = '\0';
            destination = buffer;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("SPACE", ImVec2(82.0f, 38.0f))) {
        const size_t len = std::strlen(buffer);
        if (len + 1 < capacity) {
            buffer[len] = ' ';
            buffer[len + 1] = '\0';
            destination = buffer;
            changed = true;
        }
    }

    if (ImGui::Button("BACKSPACE", ImVec2(104.0f, 38.0f))) {
        size_t len = std::strlen(buffer);
        if (len > 0) {
            buffer[len - 1] = '\0';
            destination = buffer;
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("CLEAR", ImVec2(70.0f, 38.0f))) {
        buffer[0] = '\0';
        destination.clear();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("DONE", ImVec2(62.0f, 38.0f))) {
        g_runningTextKeyboardOpen = false;
        g_touchKeyboardUppercase = false;
    }
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
    static const char* nativeScanModeNames[] = {
        "Model ID", "Texture Name"
    };
    int nativeScanMode = static_cast<int>(profile->nativeLights.scanMode);
    if (ImGui::Combo("Native RGB Scan Mode", &nativeScanMode,
                     nativeScanModeNames, IM_ARRAYSIZE(nativeScanModeNames))) {
        profile->nativeLights.scanMode =
            static_cast<VLS::NativeLightScanMode>(nativeScanMode);
        nativeLightsChanged = true;
    }
    ImGui::TextWrapped(
        "Native RGB scans each vehicle model independently. It first uses "
        "known lamp texture atlases and head/front/tail/rear frame names, "
        "then falls back to compact shallow lamp surfaces near the ends of "
        "the whole vehicle when custom models use generic names."
    );
    nativeLightsChanged |= ImGui::Checkbox("High Beam", &profile->nativeLights.highBeamEnabled);
    if (profile->nativeLights.highBeamEnabled) {
        nativeLightsChanged |= ImGui::SliderFloat("High Beam Range", &profile->nativeLights.highBeamRange, 10.0f, 500.0f);
        nativeLightsChanged |= ImGui::SliderFloat("High Beam Intensity", &profile->nativeLights.highBeamIntensity, 0.1f, 4.0f);
        ImGui::TextWrapped("Adds forward illumination while keeping the game's original low beam behavior.");
    }
    nativeLightsChanged |= ImGui::Checkbox("RGB Headlights", &profile->nativeLights.rgbHeadlightsEnabled);
    nativeLightsChanged |= ImGui::Checkbox("RGB Taillights", &profile->nativeLights.rgbTaillightsEnabled);
    if (profile->nativeLights.rgbHeadlightsEnabled || profile->nativeLights.rgbTaillightsEnabled) {
        nativeLightsChanged |= ImGui::Checkbox(
            "Vehicle Front Uses +Y Axis", &profile->nativeLights.frontAxisPositive);
        ImGui::TextWrapped("Frame names containing head/front or tail/rear/back take priority. This axis setting is used only when a model has generic lamp names.");
        nativeLightsChanged |= ImGui::SliderFloat("Native Light RGB Flow Speed", &profile->nativeLights.rgbFlowSpeed, 0.1f, 4.0f);
        nativeLightsChanged |= ImGui::SliderFloat("Native Light RGB Intensity", &profile->nativeLights.rgbFlowIntensity, 0.1f, 2.0f);
        static const char* nativeRgbBlendModeNames[] = { "Alpha", "Additive" };
        int nativeRgbBlendMode = static_cast<int>(profile->nativeLights.blendMode);
        if (ImGui::Combo("Native RGB Blend Mode", &nativeRgbBlendMode,
                         nativeRgbBlendModeNames, IM_ARRAYSIZE(nativeRgbBlendModeNames))) {
            if (nativeRgbBlendMode < 0) nativeRgbBlendMode = 0;
            if (nativeRgbBlendMode > static_cast<int>(VLS::LedLayerBlendMode::Additive))
                nativeRgbBlendMode = static_cast<int>(VLS::LedLayerBlendMode::Additive);
            profile->nativeLights.blendMode =
                static_cast<VLS::LedLayerBlendMode>(nativeRgbBlendMode);
            nativeLightsChanged = true;
        }
        ImGui::TextWrapped("Native RGB is rendered as the vehicle lighting base layer; placement layers are composited above it.");
    }
    if (nativeLightsChanged)
        g_profileManager.MarkDirty(modelId);

    // -----------------------------------------------------
    // Programmable custom sequence and transition editor
    // -----------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Custom Effect Sequence");
    if (profile->sequences.empty()) {
        if (ImGui::Button("Create Custom Sequence", ImVec2(240.0f, 42.0f))) {
            VLS::EffectSequence sequence;
            sequence.enabled = true;
            sequence.loop = true;
            if (!profile->placements.empty() &&
                !profile->placements[0].effects.empty()) {
                VLS::EffectSequenceStep step;
                step.placementId = profile->placements[0].id;
                step.effectIndex = profile->placements[0].activeEffectIndex;
                step.durationMs = 1000;
                sequence.steps.push_back(step);
            }
            profile->sequences.push_back(sequence);
            ResetSequenceRuntime();
            g_profileManager.MarkDirty(modelId);
        }
        ImGui::TextWrapped("Build a timed sequence from LED effects. Each step can target a different placement and effect.");
    } else {
        VLS::EffectSequence& sequence = profile->sequences[0];
        bool sequenceChanged = false;
        sequenceChanged |= ImGui::Checkbox("Sequence Enabled", &sequence.enabled);
        sequenceChanged |= ImGui::Checkbox("Loop Sequence", &sequence.loop);
        static const char* transitionNames[] = {
            "Crossfade", "Wipe", "Shutter", "Radial", "Pixel Dissolve"
        };
        int transitionMode = static_cast<int>(sequence.transitionMode);
        if (ImGui::Combo("Transition Mode", &transitionMode, transitionNames, IM_ARRAYSIZE(transitionNames))) {
            if (transitionMode < 0) transitionMode = 0;
            if (transitionMode > 4) transitionMode = 4;
            sequence.transitionMode = static_cast<VLS::EffectTransitionMode>(transitionMode);
            sequenceChanged = true;
        }
        static const char* transitionDirectionNames[] = {
            "Left to Right", "Right to Left", "Top to Bottom",
            "Bottom to Top", "Center Out", "Edge to Center"
        };
        int transitionDirection = static_cast<int>(sequence.transitionDirection);
        if (ImGui::Combo("Transition Direction", &transitionDirection,
                         transitionDirectionNames, IM_ARRAYSIZE(transitionDirectionNames))) {
            if (transitionDirection < 0) transitionDirection = 0;
            if (transitionDirection > 5) transitionDirection = 5;
            sequence.transitionDirection = static_cast<VLS::EffectTransitionDirection>(transitionDirection);
            sequenceChanged = true;
        }
        ImGui::Text("Sequence steps: %d / 64", static_cast<int>(sequence.steps.size()));
        if (ImGui::Button("Add Current LED Effect to Sequence", ImVec2(300.0f, 40.0f))) {
            if (sequence.steps.size() < 64 && !profile->placements.empty()) {
                const VLS::LedPlacement& target = profile->placements[
                    static_cast<size_t>(std::max(0, std::min(g_selectedPlacement,
                        static_cast<int>(profile->placements.size()) - 1)))
                ];
                if (!target.effects.empty()) {
                    VLS::EffectSequenceStep step;
                    step.placementId = target.id;
                    step.effectIndex = static_cast<uint32_t>(std::max(0, std::min(g_selectedEffect,
                        static_cast<int>(target.effects.size()) - 1)));
                    step.durationMs = 1000;
                    sequence.steps.push_back(step);
                    sequenceChanged = true;
                }
            }
        }
        if (!sequence.steps.empty()) {
            VLS::EffectSequenceStep& lastStep = sequence.steps.back();
            int duration = static_cast<int>(lastStep.durationMs);
            if (ImGui::SliderInt("Last Step Duration (ms)", &duration, 100, 30000)) {
                lastStep.durationMs = static_cast<uint32_t>(duration);
                sequenceChanged = true;
            }
            if (ImGui::Button("Remove Last Sequence Step", ImVec2(240.0f, 38.0f))) {
                sequence.steps.pop_back();
                sequenceChanged = true;
            }
        }
        if (sequenceChanged) {
            ResetSequenceRuntime();
            g_profileManager.MarkDirty(modelId);
        }
    }

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
        ImGui::TextWrapped("Strobo Eyes uses one texture-based eye artwork with three selectable geometric masks.");
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
        } else if (effect.type == VLS::LedEffectType::StroboWave) {
            effect.stroboPanelStackEnabled = true;
            VLS::InitializeStroboWavePanelDefaults(effect);
            effect.stroboPanelCount = 5;
            effect.stroboPanelColumns = 21;
            effect.stroboPanelRows = 7;
            effect.stroboPanelGapRows = 2;
            effect.matrix.enabled = true;
            effect.matrix.columns = 21;
            effect.matrix.rows = 39;
            effect.matrix.canvasEnabled = true;
            effect.matrix.canvasColumns = 21;
            effect.matrix.canvasRows = 39;
            effect.matrix.canvasOffsetColumn = 0;
            effect.matrix.canvasOffsetRow = 0;
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
        if (ImGui::Button(g_runningTextKeyboardOpen ? "Hide Virtual Keyboard" : "Open Virtual Keyboard", ImVec2(230.0f, 44.0f)))
            g_runningTextKeyboardOpen = !g_runningTextKeyboardOpen;
        if (g_runningTextKeyboardOpen)
            DrawTouchKeyboard(g_runningTextBuffer, sizeof(g_runningTextBuffer), effect.text, changed);
        changed |= ImGui::SliderFloat("Text Spacing", &effect.textSpacing, 0.25f, 3.0f);
        changed |= ImGui::SliderFloat("Font Size", &effect.textScale, 0.35f, 2.0f);
        ImGui::TextWrapped("For clear small text, keep Font Size around 0.65 or higher. Very small glyphs merge because each LED is one physical pixel; increase Matrix Columns for finer detail.");
        if (g_runningFontBufferEffect != &effect) {
            std::memset(g_runningFontBuffer, 0, sizeof(g_runningFontBuffer));
            std::strncpy(g_runningFontBuffer, effect.fontName.c_str(), sizeof(g_runningFontBuffer) - 1);
            g_runningFontBufferEffect = &effect;
        }
        static const char* fontChoices[] = {
            "Default", "Bold", "Italic", "Condensed", "Outline", "Wide", "Custom..."
        };
        static const char* fontIds[] = {
            "default", "bold", "italic", "condensed", "outline", "wide"
        };
        int fontChoice = 6;
        for (int i = 0; i < 6; ++i) {
            if (effect.fontName == fontIds[i]) { fontChoice = i; break; }
        }
        if (ImGui::Combo("Bitmap Font", &fontChoice, fontChoices, IM_ARRAYSIZE(fontChoices))) {
            if (fontChoice >= 0 && fontChoice < 6) {
                effect.fontName = fontIds[fontChoice];
            } else if (fontChoice == 6) {
                bool wasBuiltIn = false;
                for (int i = 0; i < 6; ++i)
                    wasBuiltIn = wasBuiltIn || effect.fontName == fontIds[i];
                if (wasBuiltIn)
                    effect.fontName.clear();
            }
            g_runningFontBufferEffect = nullptr;
            changed = true;
        }
        if (fontChoice == 6) {
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
        ImGui::TextWrapped("Built-in bitmap styles: Default, Bold, Italic, Condensed, Outline and Wide. Custom .vlsfont files belong in VehicleLEDSystem/fonts/.");

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
        const uint16_t minimumRows =
            effect.type == VLS::LedEffectType::StroboEyes ? 12u : 7u;
        const uint16_t minimumColumns =
            effect.type == VLS::LedEffectType::StroboEyes ? 32u : 24u;
        if (effect.matrix.rows < minimumRows) {
            effect.matrix.rows = minimumRows;
            changed = true;
        }
        if (effect.matrix.columns < minimumColumns) {
            effect.matrix.columns = minimumColumns;
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
            "Twin Spiral", "Orbit Ring",
            "Meteor Shower", "Comet Chase", "Knight Scanner", "Heartbeat",
            "Aurora Flow", "Sparkle", "Flame Flicker", "Waterfall",
            "Vortex", "Police Flash",
            "Rotating Square", "Curved Sweep", "Square Expand", "Square Collapse",
            "Circle Expand", "Circle Collapse", "Triangle Converge", "Triangle Orbit",
            "Multi-Line Flutter", "Triangle Fan Sweep", "Diagonal X Cross",
            "Multi-Axis Center Burst", "Triple Mirror Wave", "Segmented Center Expand",
            "Accumulate Left to Right", "Accumulate Right to Left",
            "Accumulate Center Out", "Accumulate Edges to Center",
            "Side Anchored - Center Slider", "Center Anchored - Side Sliders"
        };
        int motionIndex = static_cast<int>(effect.motionPattern);
        if (ImGui::Combo("Motion Pattern", &motionIndex, motionNames, IM_ARRAYSIZE(motionNames))) {
            if (motionIndex < 0) motionIndex = 0;
            if (motionIndex >= IM_ARRAYSIZE(motionNames))
                motionIndex = IM_ARRAYSIZE(motionNames) - 1;
            effect.motionPattern = static_cast<VLS::LedMotionPattern>(motionIndex);
            changed = true;
        }
        ImGui::TextWrapped("Collision / Burst: Multi Collision, Cross Burst and Radial Burst add intersecting or expanding fronts. Rebound patterns bounce after impact; Reflection patterns reverse or mirror their wave direction.");
        ImGui::TextWrapped("Laser Write draws a moving beam with a fading trace. Laser Weld adds a hot core and spark-like trail. Color Style controls the beam palette.");
        ImGui::TextWrapped("Geometry Motion: Rotating Square and Triangle Orbit trace geometric outlines; Curved Sweep bends its travelling front; Square / Circle Expand and Collapse move frames between center and edges; Triangle Converge sends three luminous points to meet at center.");
        ImGui::TextWrapped("Multi-panel motion: Multi-Line Flutter, Triangle Fan Sweep, Diagonal X Cross, Multi-Axis Center Burst, Triple Mirror Wave and Segmented Center Expand are designed to span a shared canvas across vertically stacked placements.");
        ImGui::TextWrapped("Progressive motion: Accumulate patterns add one moving LED at a time and leave each arrived LED illuminated until the canvas fills. Anchored sliders keep the selected anchor points lit while the remaining element(s) slide between or away from them.");

        if (effect.type == VLS::LedEffectType::StroboWave) {
            ImGui::Separator();
            ImGui::Text("STROBO WAVE - MULTI PANEL STACK");
            effect.stroboPanelStackEnabled = true;
            static const char* countNames[] = {"3 Panels", "5 Panels", "7 Panels"};
            int countChoice = effect.stroboPanelCount == 3 ? 0 :
                (effect.stroboPanelCount == 7 ? 2 : 1);
            if (ImGui::Combo("Strobo Panel Count", &countChoice,
                             countNames, IM_ARRAYSIZE(countNames))) {
                effect.stroboPanelCount = countChoice == 0 ? 3 :
                    (countChoice == 2 ? 7 : 5);
                changed = true;
            }
            int panelColumns = effect.stroboPanelColumns;
            int panelRows = effect.stroboPanelRows;
            int panelGap = effect.stroboPanelGapRows;
            if (ImGui::SliderInt("Panel Width (Columns)", &panelColumns, 3, 64)) changed = true;
            if (ImGui::SliderInt("Panel Height (Rows)", &panelRows, 3, 24)) changed = true;
            if (ImGui::SliderInt("Panel Gap (Rows)", &panelGap, 0, 8)) changed = true;
            // Keep the full logical grid under the 4096-pixel safety budget.
            while (static_cast<uint32_t>(panelColumns) *
                    (static_cast<uint32_t>(effect.stroboPanelCount) * panelRows +
                     static_cast<uint32_t>(effect.stroboPanelCount - 1) * panelGap) > 4096u) {
                if (panelGap > 0) --panelGap;
                else if (panelRows > 3) --panelRows;
                else if (panelColumns > 3) --panelColumns;
                else break;
            }
            effect.stroboPanelColumns = static_cast<uint16_t>(panelColumns);
            effect.stroboPanelRows = static_cast<uint16_t>(panelRows);
            effect.stroboPanelGapRows = static_cast<uint8_t>(panelGap);
            if (ImGui::Checkbox("Optional Strobe Flash", &effect.stroboFlashEnabled))
                changed = true;
            ImGui::TextWrapped("Off by default: motion patterns run continuously without an imposed on/off pulse. Enable this only when a classic blinking strobe is desired.");
            if (effect.stroboFlashEnabled) {
                int flashPeriod = static_cast<int>(effect.stroboFlashPeriodMs);
                if (ImGui::SliderInt("Flash Period (ms)", &flashPeriod, 100, 2000))
                    changed = true;
                effect.stroboFlashPeriodMs = static_cast<uint32_t>(flashPeriod);
            }

            const uint32_t totalRows =
                effect.stroboPanelCount * effect.stroboPanelRows +
                (effect.stroboPanelCount - 1u) * effect.stroboPanelGapRows;
            effect.matrix.enabled = true;
            effect.matrix.columns = effect.stroboPanelColumns;
            effect.matrix.rows = static_cast<uint16_t>(totalRows);
            effect.matrix.canvasEnabled = true;
            effect.matrix.canvasColumns = effect.stroboPanelColumns;
            effect.matrix.canvasRows = static_cast<uint16_t>(totalRows);
            effect.matrix.canvasOffsetColumn = 0;
            effect.matrix.canvasOffsetRow = 0;
            ImGui::Text("Stack: %d separate panels | %d x %d pixels each | total grid %d x %d",
                static_cast<int>(effect.stroboPanelCount),
                static_cast<int>(effect.stroboPanelColumns),
                static_cast<int>(effect.stroboPanelRows),
                static_cast<int>(effect.matrix.columns),
                static_cast<int>(effect.matrix.rows));
            static const char* compositionNames[] = {
                "Unified Geometry", "Coordinated Motion", "Independent Motion"
            };
            int compositionMode = static_cast<int>(effect.stroboCompositionMode);
            if (ImGui::Combo("Panel Motion Composition", &compositionMode,
                             compositionNames, IM_ARRAYSIZE(compositionNames))) {
                effect.stroboCompositionMode =
                    static_cast<VLS::PanelMotionCompositionMode>(compositionMode);
                changed = true;
            }
            if (effect.stroboCompositionMode ==
                VLS::PanelMotionCompositionMode::UnifiedGeometry) {
                ImGui::TextWrapped("Unified Geometry: every panel follows the main Motion Pattern as one continuous shape across the full stack. Panel phase offsets remain available; per-panel pattern, direction and density are intentionally bypassed.");
            } else if (effect.stroboCompositionMode ==
                       VLS::PanelMotionCompositionMode::CoordinatedMotion) {
                ImGui::TextWrapped("Coordinated Motion: each panel may use a different pattern. Radial / spiral / burst patterns share the complete stack's center and geometry; directional patterns retain their panel direction.");
            } else {
                ImGui::TextWrapped("Independent Motion: each panel uses its own local pattern, direction and density, without forcing a continuous global shape.");
            }
            if (effect.matrix.pixels.size() !=
                static_cast<size_t>(effect.matrix.columns) * effect.matrix.rows) {
                SyncMatrixPixels(effect);
                changed = true;
            }

            static const char* motionNames[] = {
                "Left to Right", "Right to Left", "Center Out", "Edges to Center",
                "Zoom In", "Zoom Out", "Fade In", "Fade Out", "Rolling", "Flow",
                "Wave", "Diagonal", "Ripple", "Pulse", "Alternate Rows",
                "Alternate Columns", "Checkerboard", "Windmill", "Spiral",
                "Starburst", "Laser Write", "Laser Weld", "Arrow Left", "Arrow Right",
                "Arrow Up", "Arrow Down", "Opposing Arrows", "Center Collision",
                "Center Burst", "Diagonal + Collision", "Diagonal + Center Burst",
                "Multi Collision", "Cross Burst", "Radial Burst", "Ping-Pong",
                "Mirror Reflection", "Reflective Wave", "Collision Rebound",
                "Center Split", "Dual Flow", "Mirrored Diagonal", "Double Rebound",
                "Vertical Sweep", "Side Collision", "Corner Burst", "Twin Spiral",
                "Orbit Ring", "Meteor Shower", "Comet Chase", "Knight Scanner",
                "Heartbeat", "Aurora Flow", "Sparkle", "Flame Flicker", "Waterfall",
                "Vortex", "Police Flash", "Rotating Square", "Curved Sweep",
                "Square Expand", "Square Collapse", "Circle Expand", "Circle Collapse",
                "Triangle Converge", "Triangle Orbit", "Multi-Line Flutter",
                "Triangle Fan Sweep", "Diagonal X Cross", "Multi-Axis Center Burst",
                "Triple Mirror Wave", "Segmented Center Expand",
                "Accumulate Left to Right", "Accumulate Right to Left",
                "Accumulate Center Out", "Accumulate Edges to Center",
                "Side Anchored - Center Slider", "Center Anchored - Side Sliders"
            };
            static const char* directionNames[] = {
                "Left to Right", "Right to Left", "Top to Bottom", "Bottom to Top",
                "Diagonal Down", "Diagonal Up", "Center Out", "Edges to Center"
            };
            static const char* densityNames[] = {"Adaptive", "Fixed", "Scalable", "Custom"};
            static const char* panelNames[] = {
                "Panel 1", "Panel 2", "Panel 3", "Panel 4",
                "Panel 5", "Panel 6", "Panel 7"
            };
            static int selectedPanel = 0;
            selectedPanel = std::max(0, std::min(
                selectedPanel, static_cast<int>(effect.stroboPanelCount) - 1));
            if (ImGui::Combo("Configure Panel", &selectedPanel,
                             panelNames, effect.stroboPanelCount)) {
                changed = true;
            }
            VLS::StroboWavePanelConfig& panel =
                effect.stroboPanels[static_cast<size_t>(selectedPanel)];
            changed |= ImGui::Checkbox("Panel Enabled", &panel.enabled);
            if (effect.stroboCompositionMode !=
                VLS::PanelMotionCompositionMode::UnifiedGeometry) {
                int panelMotion = static_cast<int>(panel.motionPattern);
                if (ImGui::Combo("Panel Motion Pattern", &panelMotion,
                                 motionNames, IM_ARRAYSIZE(motionNames))) {
                    panel.motionPattern = static_cast<VLS::LedMotionPattern>(panelMotion);
                    changed = true;
                }
                int panelDirection = static_cast<int>(panel.direction);
                if (ImGui::Combo("Panel Direction", &panelDirection,
                                 directionNames, IM_ARRAYSIZE(directionNames))) {
                    panel.direction = static_cast<VLS::LedColorDirection>(panelDirection);
                    changed = true;
                }
                int densityMode = static_cast<int>(panel.densityMode);
                if (ImGui::Combo("Panel Density Mode", &densityMode,
                                 densityNames, IM_ARRAYSIZE(densityNames))) {
                    panel.densityMode = static_cast<VLS::MotionDensityMode>(densityMode);
                    changed = true;
                }
                changed |= ImGui::SliderFloat("Panel Density X", &panel.densityX, 0.25f, 4.0f);
                changed |= ImGui::SliderFloat("Panel Density Y", &panel.densityY, 0.25f, 4.0f);
            }
            changed |= ImGui::SliderFloat("Panel Phase Offset", &panel.phaseOffset, -1.0f, 1.0f);
        }
    } else if (effect.type == VLS::LedEffectType::StroboEyes) {
        ImGui::Separator();
        ImGui::Text("STROBO EYES - Texture-Based Renderer");

        static const char* eyeShapeNames[] = {
            "Original / Default / Normal", "Round", "Sleepy"
        };
        int eyeShape = static_cast<int>(effect.eyeShape);
        if (ImGui::Combo("Eye Shape", &eyeShape,
                         eyeShapeNames, IM_ARRAYSIZE(eyeShapeNames))) {
            effect.eyeShape = static_cast<VLS::EyeShape>(eyeShape);
            changed = true;
        }

        static const char* eyeMotionNames[] = {
            "Idle / Natural", "Look Left", "Look Right", "Look Up", "Look Down",
            "Look Up-Left", "Look Up-Right", "Look Down-Left", "Look Down-Right",
            "Scan", "Shake", "Wink"
        };
        int eyeMotion = static_cast<int>(effect.eyeMotion);
        if (ImGui::Combo("Eye Movement", &eyeMotion, eyeMotionNames,
                         IM_ARRAYSIZE(eyeMotionNames))) {
            effect.eyeMotion = static_cast<VLS::EyeMotion>(eyeMotion);
            changed = true;
        }
        static const char* eyePlaybackNames[] = {
            "Manual", "Automatic", "Sequential", "Random"
        };
        int eyePlayback = static_cast<int>(effect.eyeMotionPlayback);
        if (ImGui::Combo("Movement Playback", &eyePlayback, eyePlaybackNames, 4)) {
            effect.eyeMotionPlayback = static_cast<VLS::MotionPlaybackMode>(eyePlayback);
            changed = true;
        }
        if (effect.eyeMotionPlayback != VLS::MotionPlaybackMode::Manual) {
            int movementInterval = static_cast<int>(effect.eyeMotionIntervalMs);
            if (ImGui::SliderInt("Movement Interval (ms)", &movementInterval, 500, 15000)) {
                effect.eyeMotionIntervalMs = static_cast<uint32_t>(movementInterval);
                changed = true;
            }
        }
        int blinkInterval = static_cast<int>(effect.eyeBlinkIntervalMs);
        if (ImGui::SliderInt("Blink Interval (ms)", &blinkInterval, 500, 8000)) {
            effect.eyeBlinkIntervalMs = static_cast<uint32_t>(blinkInterval);
            changed = true;
        }

        if (g_eyeTextureBufferEffect != &effect) {
            std::memset(g_eyeTextureBuffer, 0, sizeof(g_eyeTextureBuffer));
            std::strncpy(g_eyeTextureBuffer, effect.textureName.c_str(),
                         sizeof(g_eyeTextureBuffer) - 1);
            g_eyeTextureBufferEffect = &effect;
        }
        if (ImGui::InputText("Custom Eye Texture (PNG/PPM; blank = procedural)",
                             g_eyeTextureBuffer, sizeof(g_eyeTextureBuffer))) {
            effect.textureName = g_eyeTextureBuffer;
            changed = true;
        }
        if (ImGui::Button(g_runningTextKeyboardOpen ? "Hide Virtual Keyboard##Eye" : "Open Virtual Keyboard##Eye",
                          ImVec2(230.0f, 44.0f))) {
            g_runningTextKeyboardOpen = !g_runningTextKeyboardOpen;
            g_touchKeyboardUppercase = false;
        }
        if (g_runningTextKeyboardOpen)
            DrawTouchKeyboard(g_eyeTextureBuffer, sizeof(g_eyeTextureBuffer), effect.textureName, changed);

        // Multi-design texture set: independent from the gaze/blink controller.
        ImGui::Separator();
        ImGui::Text("CUSTOM EYE DESIGN SET");
        ImGui::TextWrapped("Enter a texture ID in the Custom Eye Texture field above, then register it here. Example: custom_eye1, custom_eye2. The existing virtual keyboard works for this field.");
        if (ImGui::Button("Register Current Texture as Design") &&
            !effect.textureName.empty() && effect.eyeTextures.size() < 32) {
            bool alreadyRegistered = false;
            for (const VLS::EyeTextureDesign& item : effect.eyeTextures)
                if (item.textureName == effect.textureName)
                    alreadyRegistered = true;
            if (!alreadyRegistered) {
                VLS::EyeTextureDesign design;
                design.name = effect.textureName;
                design.textureName = effect.textureName;
                effect.eyeTextures.push_back(design);
                effect.eyeTextureIndex = static_cast<uint8_t>(effect.eyeTextures.size() - 1);
                changed = true;
            }
        }
        if (effect.eyeTextures.empty()) {
            ImGui::TextWrapped("No design registered. Add IDs such as custom_eye1, custom_eye2; place matching PNG/PPM files in VehicleLEDSystem/textures.");
        } else {
            static const char* texturePlaybackNames[] = {
                "Manual", "Automatic", "Sequence", "Random"
            };
            int texturePlayback = static_cast<int>(effect.eyeTexturePlayback);
            if (ImGui::Combo("Design Playback", &texturePlayback,
                             texturePlaybackNames, IM_ARRAYSIZE(texturePlaybackNames))) {
                effect.eyeTexturePlayback =
                    static_cast<VLS::EyeTexturePlaybackMode>(texturePlayback);
                changed = true;
            }
            int textureInterval = static_cast<int>(effect.eyeTextureIntervalMs);
            if (ImGui::SliderInt("Design Change Interval (ms)",
                                 &textureInterval, 500, 30000)) {
                effect.eyeTextureIntervalMs = static_cast<uint32_t>(textureInterval);
                changed = true;
            }
            std::vector<std::string> designLabels;
            std::vector<const char*> designLabelPointers;
            designLabels.reserve(effect.eyeTextures.size());
            designLabelPointers.reserve(effect.eyeTextures.size());
            for (const VLS::EyeTextureDesign& design : effect.eyeTextures) {
                designLabels.push_back(design.name + " (" + design.textureName + ")");
            }
            for (const std::string& label : designLabels)
                designLabelPointers.push_back(label.c_str());
            int selectedDesign = effect.eyeTextureIndex < effect.eyeTextures.size()
                ? effect.eyeTextureIndex : 0;
            if (ImGui::Combo("Selected Design / Manual Index", &selectedDesign,
                             designLabelPointers.data(),
                             static_cast<int>(designLabelPointers.size()))) {
                effect.eyeTextureIndex = static_cast<uint8_t>(selectedDesign);
                changed = true;
            }
            if (ImGui::Button("Remove Selected Design")) {
                effect.eyeTextures.erase(effect.eyeTextures.begin() + selectedDesign);
                for (size_t i = 0; i < effect.eyeTextureSequence.size();) {
                    if (effect.eyeTextureSequence[i] == selectedDesign) {
                        effect.eyeTextureSequence.erase(effect.eyeTextureSequence.begin() + i);
                    } else {
                        if (effect.eyeTextureSequence[i] > selectedDesign)
                            --effect.eyeTextureSequence[i];
                        ++i;
                    }
                }
                if (effect.eyeTextures.empty()) {
                    effect.eyeTextureIndex = 0;
                    effect.eyeTextureSequence.clear();
                } else if (effect.eyeTextureIndex >= effect.eyeTextures.size()) {
                    effect.eyeTextureIndex = static_cast<uint8_t>(effect.eyeTextures.size() - 1);
                }
                selectedDesign = effect.eyeTextureIndex;
                changed = true;
            }
            if (!effect.eyeTextures.empty() &&
                effect.eyeTexturePlayback == VLS::EyeTexturePlaybackMode::Sequence) {
                ImGui::TextWrapped("Sequence follows the saved index list. With an empty list it cycles through designs in registration order.");
                if (ImGui::Button("Append Selected to Sequence") &&
                    effect.eyeTextureSequence.size() < 32) {
                    effect.eyeTextureSequence.push_back(static_cast<uint8_t>(selectedDesign));
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove Last Sequence Step") &&
                    !effect.eyeTextureSequence.empty()) {
                    effect.eyeTextureSequence.pop_back();
                    changed = true;
                }
                if (!effect.eyeTextureSequence.empty()) {
                    ImGui::Text("Sequence:");
                    for (size_t i = 0; i < effect.eyeTextureSequence.size(); ++i) {
                        ImGui::SameLine();
                        ImGui::Text("%u", static_cast<unsigned int>(
                            effect.eyeTextureSequence[i] + 1));
                    }
                }
            }
            if (!effect.eyeTextures.empty()) {
                selectedDesign = std::min(selectedDesign,
                    static_cast<int>(effect.eyeTextures.size() - 1));
                VLS::EyeTextureDesign& selected = effect.eyeTextures[selectedDesign];
                if (ImGui::CollapsingHeader("Selected Design UV Mapping")) {
                    bool mappingChanged = false;
                    mappingChanged |= ImGui::SliderFloat("Design Iris Center X", &selected.irisCenterX, 0.0f, 1.0f);
                    mappingChanged |= ImGui::SliderFloat("Design Iris Center Y", &selected.irisCenterY, 0.0f, 1.0f);
                    mappingChanged |= ImGui::SliderFloat("Design Iris Radius X", &selected.irisRadiusX, 0.02f, 0.50f);
                    mappingChanged |= ImGui::SliderFloat("Design Iris Radius Y", &selected.irisRadiusY, 0.02f, 0.50f);
                    mappingChanged |= ImGui::SliderFloat("Design Pupil Center X", &selected.pupilCenterX, 0.0f, 1.0f);
                    mappingChanged |= ImGui::SliderFloat("Design Pupil Center Y", &selected.pupilCenterY, 0.0f, 1.0f);
                    mappingChanged |= ImGui::SliderFloat("Design Pupil Radius X", &selected.pupilRadiusX, 0.01f, 0.30f);
                    mappingChanged |= ImGui::SliderFloat("Design Pupil Radius Y", &selected.pupilRadiusY, 0.01f, 0.30f);
                    if (mappingChanged) {
                        selected.mappingConfigured = true;
                        changed = true;
                    }
                    if (!selected.mappingConfigured)
                        ImGui::TextWrapped("No manual UV map saved. VLS first attempts conservative detection of sclera, iris, pupil and eyebrow from the PNG. If confidence is insufficient, the artwork stays still; set the iris and pupil UV regions here to calibrate it.");
                }
            }
        }

        if (effect.textureName.empty() && effect.eyeTextures.empty()) {
            float irisColor[3] = {
                effect.eyeIrisRed / 255.0f, effect.eyeIrisGreen / 255.0f,
                effect.eyeIrisBlue / 255.0f
            };
            if (ImGui::ColorEdit3("Iris Color", irisColor)) {
                effect.eyeIrisRed = static_cast<uint8_t>(irisColor[0] * 255.0f);
                effect.eyeIrisGreen = static_cast<uint8_t>(irisColor[1] * 255.0f);
                effect.eyeIrisBlue = static_cast<uint8_t>(irisColor[2] * 255.0f);
                changed = true;
            }
            float pupilColor[3] = {
                effect.eyePupilRed / 255.0f, effect.eyePupilGreen / 255.0f,
                effect.eyePupilBlue / 255.0f
            };
            if (ImGui::ColorEdit3("Pupil Color", pupilColor)) {
                effect.eyePupilRed = static_cast<uint8_t>(pupilColor[0] * 255.0f);
                effect.eyePupilGreen = static_cast<uint8_t>(pupilColor[1] * 255.0f);
                effect.eyePupilBlue = static_cast<uint8_t>(pupilColor[2] * 255.0f);
                changed = true;
            }
            ImGui::TextWrapped("Procedural mode: the eye engine draws the iris and pupil and applies gaze movement. The three palettes remain independent.");
        } else {
            ImGui::TextWrapped("Custom mode preserves PNG/PPM colors and alpha. VLS keeps the eyebrow and eyeball artwork stationary, moves the mapped iris and pupil independently, and closes blink/wink with an aperture mask rather than zooming or compressing the full texture. Mapping values are saved with this vehicle profile.");
            if (ImGui::CollapsingHeader("Custom Eye Mapping")) {
                ImGui::TextWrapped("Adjust these normalized regions to match the iris and pupil positions in each half of your two-eye PNG.");
                changed |= ImGui::SliderFloat("Iris Center X", &effect.eyeTextureIrisCenterX, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Iris Center Y", &effect.eyeTextureIrisCenterY, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Iris Radius X", &effect.eyeTextureIrisRadiusX, 0.02f, 0.50f);
                changed |= ImGui::SliderFloat("Iris Radius Y", &effect.eyeTextureIrisRadiusY, 0.02f, 0.50f);
                changed |= ImGui::SliderFloat("Pupil Center X", &effect.eyeTexturePupilCenterX, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Pupil Center Y", &effect.eyeTexturePupilCenterY, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Pupil Radius X", &effect.eyeTexturePupilRadiusX, 0.01f, 0.30f);
                changed |= ImGui::SliderFloat("Pupil Radius Y", &effect.eyeTexturePupilRadiusY, 0.01f, 0.30f);
            }
        }
        ImGui::TextWrapped("Use a two-eye PNG with the left and right eye in separate horizontal halves, or P3 PPM, in VehicleLEDSystem/textures/ (maximum 1024x1024). Expression presets are removed; gaze and blink remain.");
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

    ImGui::Separator();
    ImGui::Text("LED Element Shape");
    static const char* elementShapeNames[] = {
        "Square", "Circle Filled", "Circle Outline", "Diamond",
        "Triangle", "Hexagon", "Octagon"
    };
    int elementShape = static_cast<int>(effect.elementShape);
    if (ImGui::Combo("Element Shape", &elementShape,
                     elementShapeNames, IM_ARRAYSIZE(elementShapeNames))) {
        if (elementShape < 0 || elementShape > static_cast<int>(VLS::LedElementShape::Octagon))
            elementShape = static_cast<int>(VLS::LedElementShape::Square);
        effect.elementShape = static_cast<VLS::LedElementShape>(elementShape);
        changed = true;
    }
    ImGui::TextWrapped("Changes the physical outline of each LED/pixel. The placement panel remains rectangular.");

    ImGui::Separator();
    ImGui::Text("Per-Effect Lighting (independent for this effect)");
    changed |= ImGui::Checkbox("Corona##effectLighting", &effect.coronaEnabled);
    changed |= ImGui::Checkbox("Glow##effectLighting", &effect.glowEnabled);
    changed |= ImGui::Checkbox("Light Spread##effectLighting", &effect.lightSpreadEnabled);
    if (effect.coronaEnabled) {
        float value = effect.coronaIntensity;
        if (ImGui::SliderFloat("Corona Intensity", &value, 0.0f, 2.0f)) {
            effect.coronaIntensity = value;
            changed = true;
        }
    }
    if (effect.glowEnabled) {
        float value = effect.glowIntensity;
        if (ImGui::SliderFloat("Glow Intensity", &value, 0.0f, 2.0f)) {
            effect.glowIntensity = value;
            changed = true;
        }
    }
    if (effect.lightSpreadEnabled) {
        float value = effect.lightSpreadIntensity;
        if (ImGui::SliderFloat("Light Spread Intensity", &value, 0.0f, 2.0f)) {
            effect.lightSpreadIntensity = value;
            changed = true;
        }
    }
    ImGui::TextWrapped("Each effect/layer has independent switches. Disabled effects generate no extra lighting geometry.");

    ImGui::Separator();
    ImGui::Text("Layer Composition");
    int layerOrder = effect.layerOrder;
    if (ImGui::SliderInt("Layer Order", &layerOrder, 0, 255)) {
        effect.layerOrder = static_cast<uint8_t>(layerOrder);
        changed = true;
    }
    const char* blendModes[] = {"Alpha", "Additive"};
    int blendMode = static_cast<int>(effect.blendMode);
    if (ImGui::Combo("Layer Blend Mode", &blendMode, blendModes, 2)) {
        effect.blendMode = static_cast<VLS::LedLayerBlendMode>(blendMode);
        changed = true;
    }
    ImGui::TextWrapped("Lower layer order is drawn first. Alpha overlays preserve normal transparency; Additive layers accumulate light for glow and multi-effect combinations.");
    ImGui::TextWrapped("Each enabled layer now runs its own selected motion pattern and timing independently. Select another effect in this placement to configure its motion.");

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

    if (effect.type != VLS::LedEffectType::StroboEyes ||
        (effect.textureName.empty() && effect.eyeTextures.empty())) {
        float effectColor[3] = {
            effect.red / 255.0f,
            effect.green / 255.0f,
            effect.blue / 255.0f
        };
        const char* effectColorLabel =
            effect.type == VLS::LedEffectType::StroboEyes
                ? "Eyeball Color" : "Effect Color";
        if (ImGui::ColorEdit3(effectColorLabel, effectColor)) {
            effect.red = static_cast<uint8_t>(effectColor[0] * 255.0f);
            effect.green = static_cast<uint8_t>(effectColor[1] * 255.0f);
            effect.blue = static_cast<uint8_t>(effectColor[2] * 255.0f);
            changed = true;
        }
    }

    // Shared logical canvas is independent of Matrix mode. Strip-based
    // Running LED, RGB and Strobo Wave can therefore join the same field.
    ImGui::Separator();
    ImGui::Text("Shared Animation Canvas");
    changed |= ImGui::Checkbox("Use Shared Canvas", &effect.matrix.canvasEnabled);
    if (effect.matrix.canvasEnabled) {
        int canvasColumns = effect.matrix.canvasColumns;
        int canvasRows = effect.matrix.canvasRows;
        int offsetColumn = effect.matrix.canvasOffsetColumn;
        int offsetRow = effect.matrix.canvasOffsetRow;
        const int regionColumns = effect.matrix.enabled
            ? static_cast<int>(effect.matrix.columns) : 12;
        const int regionRows = effect.matrix.enabled
            ? static_cast<int>(effect.matrix.rows) : 1;

        // Touch-friendly steppers replace the narrow ImGui sliders on mobile.
        // Canvas dimensions use +/-; region offsets use directional controls.
        auto drawCanvasStepper = [&](const char* label, const char* id,
                                     int& value, int minimum, int maximum,
                                     const char* decrement,
                                     const char* increment) {
            ImGui::PushID(id);
            ImGui::Text("%s: %d", label, value);
            if (ImGui::Button(decrement, ImVec2(52.0f, 42.0f))) {
                value = std::max(minimum, value - 1);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::Text(" %d ", value);
            ImGui::SameLine();
            if (ImGui::Button(increment, ImVec2(52.0f, 42.0f))) {
                value = std::min(maximum, value + 1);
                changed = true;
            }
            ImGui::PopID();
        };

        // Quick layout helper for physically stacked LED panels.
        // Each placement selects its own vertical slot while sharing one
        // virtual canvas. Unequal panel dimensions remain configurable below.
        ImGui::Separator();
        ImGui::Text("Vertical Panel Stack (3-7 Panels)");
        int stackPanelCount = regionRows > 0
            ? canvasRows / regionRows : 3;
        if (stackPanelCount < 3 || stackPanelCount > 7)
            stackPanelCount = 3;
        const char* stackPanelCountNames[] = {
            "3 Panels", "4 Panels", "5 Panels", "6 Panels", "7 Panels"
        };
        int stackPanelCountChoice = stackPanelCount - 3;
        if (ImGui::Combo("Stack Size", &stackPanelCountChoice,
                         stackPanelCountNames, IM_ARRAYSIZE(stackPanelCountNames))) {
            stackPanelCount = stackPanelCountChoice + 3;
            canvasRows = std::min(256, regionRows * stackPanelCount);
            changed = true;
        }
        int stackPanelIndex = regionRows > 0 ? offsetRow / regionRows : 0;
        stackPanelIndex = std::max(0, std::min(stackPanelIndex, stackPanelCount - 1));
        const char* stackPositionNames[] = {
            "Panel 1 (Top)", "Panel 2", "Panel 3", "Panel 4",
            "Panel 5", "Panel 6", "Panel 7 (Bottom)"
        };
        if (ImGui::Combo("This Placement", &stackPanelIndex,
                         stackPositionNames, stackPanelCount)) {
            offsetRow = stackPanelIndex * regionRows;
            changed = true;
        }
        ImGui::TextWrapped("Use the same Stack Size on every participating placement. Select a different This Placement value for each physical panel. Each panel keeps its own vehicle position and dimensions.");
        ImGui::Separator();

        drawCanvasStepper("Canvas Columns", "CanvasColumns",
                          canvasColumns, regionColumns, 256, "-", "+");
        drawCanvasStepper("Canvas Rows", "CanvasRows",
                          canvasRows, regionRows, 256, "-", "+");
        drawCanvasStepper("Region X Offset", "RegionXOffset",
                          offsetColumn, 0, 255, "<", ">");
        drawCanvasStepper("Region Y Offset", "RegionYOffset",
                          offsetRow, 0, 255, "-", "+");
        canvasColumns = std::max(canvasColumns, regionColumns);
        canvasRows = std::max(canvasRows, regionRows);
        offsetColumn = std::max(0, std::min(offsetColumn, canvasColumns - regionColumns));
        offsetRow = std::max(0, std::min(offsetRow, canvasRows - regionRows));
        effect.matrix.canvasColumns = static_cast<uint16_t>(canvasColumns);
        effect.matrix.canvasRows = static_cast<uint16_t>(canvasRows);
        effect.matrix.canvasOffsetColumn = static_cast<uint16_t>(offsetColumn);
        effect.matrix.canvasOffsetRow = static_cast<uint16_t>(offsetRow);
        ImGui::TextWrapped("Use identical canvas dimensions on every participating region. Set each region's X/Y offset. Strip effects use a 12x1 local region; matrix effects use their configured columns and rows. All regions sample one continuous animation field.");
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

            static const char* pixelShapeNames[] = {
                "Use Effect Shape", "Square", "Circle Filled",
                "Circle Outline", "Diamond", "Triangle", "Hexagon", "Octagon"
            };
            int pixelShapeIndex = pixel.elementShape == VLS::LedElementShape::Inherit
                ? 0 : static_cast<int>(pixel.elementShape) + 1;
            if (ImGui::Combo("Pixel Element Shape", &pixelShapeIndex,
                    pixelShapeNames, 8)) {
                pixel.elementShape = pixelShapeIndex == 0
                    ? VLS::LedElementShape::Inherit
                    : static_cast<VLS::LedElementShape>(pixelShapeIndex - 1);
                changed = true;
            }
            ImGui::TextWrapped("Each matrix cell can override the effect shape, allowing circles, squares and other geometry in one pattern.");

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
            // One immutable timestamp synchronizes Native RGB and all LED layers.
            const uint32_t frameTimeMs = CTimer::GetTimeMS();
            if (g_geometryScanSucceeded &&
                g_geometryScanVehicle == currentVehicle &&
                g_geometryScanModelId == modelId &&
                currentVehicle->m_pRwClump != nullptr &&
                reinterpret_cast<uintptr_t>(
                    currentVehicle->m_pRwClump
                ) == g_geometryScanClumpIdentity &&
                (profile->nativeLights.rgbHeadlightsEnabled ||
                 profile->nativeLights.rgbTaillightsEnabled)) {
                VLS::NativeLightRenderStats nativeStats;
                const uint32_t renderedPatches =
                    VLS::ProfileRenderer::RenderNativeLights(
                        currentVehicle, *profile, frameTimeMs, &nativeStats
                    );
                if (!g_nativeRgbRenderDiagnosticLogged) {
                    const auto& patches =
                        VLS::VehicleGeometryReader::GetSurfacePatches();
                    uint32_t lightingPatches = 0;
                    uint32_t frontPatches = 0;
                    uint32_t rearPatches = 0;
                    for (const VLS::SurfacePatch& patch : patches) {
                        if (patch.role != VLS::SurfaceRoleCandidate::Lighting)
                            continue;
                        ++lightingPatches;
                        if (patch.centroid.y > 0.25f) ++frontPatches;
                        else if (patch.centroid.y < -0.25f) ++rearPatches;
                    }
                    char diagnostic[1024];
                    const int diagnosticLength = std::snprintf(
                        diagnostic,
                        sizeof(diagnostic),
                        "VLS Native RGB Render Diagnostic\n"
                        "Model ID: %d\n"
                        "Lighting patches: %u\n"
                        "Front patches: %u\n"
                        "Rear patches: %u\n"
                        "Rendered patches: %u\n"
                        "Unclassified side patches: %u\n"
                        "Side disabled: %u\n"
                        "Missing mesh: %u\n"
                        "Invalid geometry: %u\n"
                        "Missing vehicle matrix: %u\n"
                        "Empty triangle output: %u\n"
                        "Transform failed: %u\n"
                        "Draw failed: %u\n"
                        "Submitted vertices: %u\n"
                        "Submitted indices: %u\n"
                        "Valid triangles: %u\n"
                        "Alpha: %u\n",
                        modelId,
                        static_cast<unsigned int>(lightingPatches),
                        static_cast<unsigned int>(frontPatches),
                        static_cast<unsigned int>(rearPatches),
                        static_cast<unsigned int>(renderedPatches),
                        static_cast<unsigned int>(nativeStats.unclassifiedSidePatches),
                        static_cast<unsigned int>(nativeStats.sideDisabled),
                        static_cast<unsigned int>(nativeStats.missingMesh),
                        static_cast<unsigned int>(nativeStats.invalidGeometry),
                        static_cast<unsigned int>(nativeStats.missingVehicleMatrix),
                        static_cast<unsigned int>(nativeStats.emptyTriangleOutput),
                        static_cast<unsigned int>(nativeStats.transformFailed),
                        static_cast<unsigned int>(nativeStats.drawFailed),
                        static_cast<unsigned int>(nativeStats.submittedVertices),
                        static_cast<unsigned int>(nativeStats.submittedIndices),
                        static_cast<unsigned int>(nativeStats.validTriangles),
                        static_cast<unsigned int>(nativeStats.alpha)
                    );
                    if (diagnosticLength > 0 &&
                        static_cast<size_t>(diagnosticLength) < sizeof(diagnostic)) {
                        char diagnosticFile[64];
                        std::snprintf(
                            diagnosticFile, sizeof(diagnosticFile),
                            "native_rgb_render_%d.txt", modelId
                        );
                        VLS::ProfileStorage::SaveDiagnosticText(
                            diagnosticFile,
                            std::string(diagnostic, static_cast<size_t>(diagnosticLength))
                        );
                    }
                    logger->Info(
                        "VLS native RGB render: patches=%u front=%u rear=%u rendered=%u sideUnknown=%u sideOff=%u noMesh=%u badGeo=%u noMatrix=%u empty=%u transformFail=%u drawFail=%u verts=%u indices=%u triangles=%u alpha=%u",
                        static_cast<unsigned int>(lightingPatches),
                        static_cast<unsigned int>(frontPatches),
                        static_cast<unsigned int>(rearPatches),
                        static_cast<unsigned int>(renderedPatches),
                        static_cast<unsigned int>(nativeStats.unclassifiedSidePatches),
                        static_cast<unsigned int>(nativeStats.sideDisabled),
                        static_cast<unsigned int>(nativeStats.missingMesh),
                        static_cast<unsigned int>(nativeStats.invalidGeometry),
                        static_cast<unsigned int>(nativeStats.missingVehicleMatrix),
                        static_cast<unsigned int>(nativeStats.emptyTriangleOutput),
                        static_cast<unsigned int>(nativeStats.transformFailed),
                        static_cast<unsigned int>(nativeStats.drawFailed),
                        static_cast<unsigned int>(nativeStats.submittedVertices),
                        static_cast<unsigned int>(nativeStats.submittedIndices),
                        static_cast<unsigned int>(nativeStats.validTriangles),
                        static_cast<unsigned int>(nativeStats.alpha)
                    );
                    g_nativeRgbRenderDiagnosticLogged = true;
                }
            }
            const VLS::EffectSequenceStep* activeSequenceStep =
                g_sequenceRuntime.GetCurrentStep(*profile);
            const VLS::EffectSequenceStep* previousSequenceStep =
                g_sequenceRuntime.GetPreviousStep(*profile, frameTimeMs);
            const float sequenceTransitionProgress =
                g_sequenceRuntime.GetTransitionProgress(frameTimeMs);
            const VLS::EffectTransitionMode sequenceTransitionMode =
                g_sequenceRuntime.GetTransitionMode(*profile);
            const VLS::EffectTransitionDirection sequenceTransitionDirection =
                g_sequenceRuntime.GetTransitionDirection(*profile);
            for (CVehicle* vehicle : g_frameVehicles) {
                if (vehicle != currentVehicle || !IsVehiclePointerValid(vehicle))
                    continue;

                // Custom sequence changes crossfade outgoing and incoming
                // effects using the same timestamp as the rest of this frame.
                VLS::ProfileRenderer::RenderVehicleProfile(
                    vehicle, *profile, frameTimeMs, activeSequenceStep,
                    previousSequenceStep, sequenceTransitionProgress,
                    sequenceTransitionMode, sequenceTransitionDirection
                );
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
    g_geometryScanMode = VLS::NativeLightScanMode::ModelId;
    g_geometryScanSucceeded = false;
    g_geometryScanAttemptComplete = false;
    g_geometryScanClumpUnavailable = false;
    g_geometryScanClumpIdentity = 0;
    g_geometryScanDiagnosticLogged = false;
    g_nativeRgbRenderDiagnosticLogged = false;
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

    const VLS::VehicleProfile* activeProfile =
        g_profileManager.Find(modelId);
    const VLS::NativeLightScanMode scanMode = activeProfile != nullptr
        ? activeProfile->nativeLights.scanMode
        : VLS::NativeLightScanMode::ModelId;

    if (vehicle != g_geometryScanVehicle ||
        modelId != g_geometryScanModelId ||
        scanMode != g_geometryScanMode) {
        VLS::VehicleGeometryReader::Clear();
        g_geometryScanVehicle = vehicle;
        g_geometryScanModelId = modelId;
        g_geometryScanMode = scanMode;
        g_geometryScanSucceeded = false;
        g_geometryScanAttemptComplete = false;
        g_geometryScanClumpUnavailable = false;
        g_geometryScanDiagnosticLogged = false;
        g_nativeRgbRenderDiagnosticLogged = false;
    }

    // A clump can temporarily disappear during model streaming. Never
    // leave a completed cache active while its source clump is unavailable.
    if (vehicle->m_pRwClump == nullptr) {
        g_geometryScanSucceeded = false;
        if (!g_geometryScanClumpUnavailable) {
            VLS::VehicleGeometryReader::Clear();
            g_geometryScanAttemptComplete = false;
            g_geometryScanClumpUnavailable = true;
            g_geometryScanClumpIdentity = 0;
        }

        return;
    }

    if (g_geometryScanClumpUnavailable) {
        VLS::VehicleGeometryReader::Clear();
        g_geometryScanSucceeded = false;
        g_geometryScanAttemptComplete = false;
        g_geometryScanClumpUnavailable = false;
        g_geometryScanClumpIdentity = 0;
    }

    const uintptr_t currentClumpIdentity =
        reinterpret_cast<uintptr_t>(vehicle->m_pRwClump);
    if (g_geometryScanClumpIdentity != 0 &&
        g_geometryScanClumpIdentity != currentClumpIdentity) {
        VLS::VehicleGeometryReader::Clear();
        g_geometryScanSucceeded = false;
        g_geometryScanAttemptComplete = false;
    }
    g_geometryScanClumpIdentity = currentClumpIdentity;

    if (g_geometryScanAttemptComplete)
        return;

    VLS::GeometryScanStats stats;
    bool complete = false;
    const bool succeeded =
        VLS::VehicleGeometryReader::ScanIncremental(
            vehicle,
            modelId,
            stats,
            complete,
            scanMode == VLS::NativeLightScanMode::TextureName
        );

    if (complete) {
        g_geometryScanAttemptComplete = true;
        g_geometryScanSucceeded = succeeded;
        if (!g_geometryScanDiagnosticLogged) {
            const auto& meshes = VLS::VehicleGeometryReader::GetMeshes();
            const auto& patches = VLS::VehicleGeometryReader::GetSurfacePatches();
            uint32_t lightingPatches = 0;
            for (const VLS::SurfacePatch& patch : patches) {
                if (patch.role == VLS::SurfaceRoleCandidate::Lighting)
                    ++lightingPatches;
            }
            char diagnostic[768];
            const int diagnosticLength = std::snprintf(
                diagnostic,
                sizeof(diagnostic),
                "VLS Native RGB Geometry Scan Diagnostic\n"
                "Model ID: %d\n"
                "Scan success: %d\n"
                "Failure reason: %s\n"
                "Atomic count: %u\n"
                "Geometry count: %u\n"
                "CPU vertex geometries: %u\n"
                "Native geometries: %u\n"
                "Vertex-buffer geometries: %u\n"
                "Missing vertex data: %u\n"
                "Missing triangle data: %u\n"
                "Skipped vertex limits: %u\n"
                "Skipped invalid vertices: %u\n"
                "Skipped without CPU vertices: %u\n"
                "Skipped without triangle data: %u\n"
                "Skipped without valid triangles: %u\n"
                "Invalid triangle indices: %llu\n"
                "Cached meshes: %u\n"
                "Cached vertices: %llu\n"
                "Cached triangles: %llu\n"
                "Surface patches: %u\n"
                "Lighting patches: %u\n",
                modelId,
                succeeded ? 1 : 0,
                stats.failureReason[0] != '\0'
                    ? stats.failureReason : "none",
                static_cast<unsigned int>(stats.atomicCount),
                static_cast<unsigned int>(stats.geometryCount),
                static_cast<unsigned int>(stats.cpuVertexGeometryCount),
                static_cast<unsigned int>(stats.nativeGeometryCount),
                static_cast<unsigned int>(stats.vertexBufferGeometryCount),
                static_cast<unsigned int>(stats.missingVertexDataCount),
                static_cast<unsigned int>(stats.missingTriangleDataCount),
                static_cast<unsigned int>(stats.skippedVertexLimitCount),
                static_cast<unsigned int>(stats.skippedInvalidVertexCount),
                static_cast<unsigned int>(stats.skippedNoCpuVertexCount),
                static_cast<unsigned int>(stats.skippedNoTriangleDataCount),
                static_cast<unsigned int>(stats.skippedNoValidTriangleCount),
                static_cast<unsigned long long>(stats.invalidTriangleIndexCount),
                static_cast<unsigned int>(meshes.size()),
                static_cast<unsigned long long>(stats.cachedVertexCount),
                static_cast<unsigned long long>(stats.cachedTriangleCount),
                static_cast<unsigned int>(patches.size()),
                static_cast<unsigned int>(lightingPatches)
            );
            if (diagnosticLength > 0 &&
                static_cast<size_t>(diagnosticLength) < sizeof(diagnostic)) {
                std::string diagnosticText(
                    diagnostic, static_cast<size_t>(diagnosticLength)
                );
                diagnosticText += "\nCACHED MESH / MATERIAL DETAILS\n";
                for (const VLS::AtomicMesh& mesh : meshes) {
                    uint32_t meshPatches = 0;
                    uint32_t meshLightingPatches = 0;
                    for (const VLS::SurfacePatch& patch : patches) {
                        if (patch.atomicIndex != mesh.atomicIndex)
                            continue;
                        ++meshPatches;
                        if (patch.role == VLS::SurfaceRoleCandidate::Lighting)
                            ++meshLightingPatches;
                    }
                    char meshLine[256];
                    const int meshLineLength = std::snprintf(
                        meshLine, sizeof(meshLine),
                        "Atomic %u Geometry %u: vertices=%u triangles=%u patches=%u lighting=%u\n",
                        static_cast<unsigned int>(mesh.atomicIndex),
                        static_cast<unsigned int>(mesh.geometryIndex),
                        static_cast<unsigned int>(mesh.vertices.size()),
                        static_cast<unsigned int>(mesh.triangles.size()),
                        static_cast<unsigned int>(meshPatches),
                        static_cast<unsigned int>(meshLightingPatches)
                    );
                    if (meshLineLength > 0 &&
                        static_cast<size_t>(meshLineLength) < sizeof(meshLine))
                        diagnosticText.append(meshLine, static_cast<size_t>(meshLineLength));
                    for (const VLS::MeshMaterialInfo& material : mesh.materials) {
                        char materialLine[160];
                        const int materialLineLength = std::snprintf(
                            materialLine, sizeof(materialLine),
                            "  Material %d: texture='%s' mask='%s'\n",
                            static_cast<int>(material.index),
                            material.textureName.c_str(),
                            material.maskName.c_str()
                        );
                        if (materialLineLength > 0 &&
                            static_cast<size_t>(materialLineLength) < sizeof(materialLine))
                            diagnosticText.append(
                                materialLine, static_cast<size_t>(materialLineLength)
                            );
                    }
                }
                char diagnosticFile[64];
                std::snprintf(
                    diagnosticFile, sizeof(diagnosticFile),
                    "native_rgb_scan_%d.txt", modelId
                );
                VLS::ProfileStorage::SaveDiagnosticText(
                    diagnosticFile,
                    diagnosticText
                );
            }
            logger->Info(
                "VLS native RGB scan: success=%d atomics=%u meshes=%u vertices=%llu triangles=%llu patches=%u lighting=%u skippedNoCpu=%u",
                succeeded ? 1 : 0,
                static_cast<unsigned int>(stats.atomicCount),
                static_cast<unsigned int>(meshes.size()),
                static_cast<unsigned long long>(stats.cachedVertexCount),
                static_cast<unsigned long long>(stats.cachedTriangleCount),
                static_cast<unsigned int>(patches.size()),
                static_cast<unsigned int>(lightingPatches),
                static_cast<unsigned int>(stats.skippedNoCpuVertexCount)
            );
            g_geometryScanDiagnosticLogged = true;
        }
    }
}


// ---------------------------------------------------------
// Sequence runtime
// ---------------------------------------------------------

static void UpdateSequenceRuntime() {
    if (g_currentProfile == nullptr)
        return;

    // An enabled profile sequence is authoritative; do not require the
    // legacy global animation-mode switch (which is not exposed by the editor).
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
