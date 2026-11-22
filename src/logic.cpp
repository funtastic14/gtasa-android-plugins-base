
#include "logic.h"

#include "vehicle_manager.h"
#include "vehicle_profile_manager.h"
#include "profile_storage.h"
#include "profile_sequence_runtime.h"
#include "profile_renderer.h"
#include "vehicle_geometry_reader.h"
#include "animation_controller.h"
#include "vehicle_profile.h"

#include <mod/config.h>
#include <mod/logger.h>

#include <plugin-sdk/game_sa/Events.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/renderware/imgui/imgui_impl_renderware.h>

#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace Logic {

static bool g_enabled = true;
static bool g_editorOpen = false;
static bool g_imguiInitialized = false;

static int g_brightness = 100;
static float g_animationSpeed = 1.0f;

static int g_selectedPixel = 0;
static int g_selectedPlacement = 0;

static VLS::VehicleManager g_vehicleManager;
static VLS::VehicleProfileManager g_profileManager;
static VLS::ProfileSequenceRuntime g_sequenceRuntime;
static VLS::AnimationController g_animationController;

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

static const char* g_zoneNames[] = {
    "Windshield",
    "Exterior Body",
    "Headlight",
    "Taillight",
    "Interior"
};

static const VLS::LedZone g_zoneTypes[] = {
    VLS::LedZone::Windshield,
    VLS::LedZone::ExteriorBody,
    VLS::LedZone::Headlight,
    VLS::LedZone::Taillight,
    VLS::LedZone::Interior
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
    "Automatic",
    "Random",
    "Sequential",
    "Custom"
};

static const VLS::AnimationMode g_animationModeTypes[] = {
    VLS::AnimationMode::Manual,
    VLS::AnimationMode::Automatic,
    VLS::AnimationMode::Random,
    VLS::AnimationMode::Sequential,
    VLS::AnimationMode::Custom
};

static int GetPatternIndex(VLS::LedEffectType type) {
    for (int i = 0; i < 4; ++i) {
        if (g_patternTypes[i] == type)
            return i;
    }

    return 0;
}

static int GetZoneIndex(VLS::LedZone zone) {
    for (int i = 0; i < 5; ++i) {
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

    if (g_animationMode == VLS::AnimationMode::Custom)
        return;

    g_animationController.SetIntervalMs(
        g_animationIntervalMs
    );

    g_animationController.Update(
        *g_currentProfile,
        CTimer::GetTimeMS()
    );
}


// ---------------------------------------------------------
// ImGui launcher and editor
// ---------------------------------------------------------

static bool DrawSpacedFloatControl(
    const char* label, float* value, float minValue, float maxValue, float step
) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::PushItemWidth(-1.0f);
    changed |= ImGui::SliderFloat(label, value, minValue, maxValue, "%.3f");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) {
        *value -= step;
        if (*value < minValue) *value = minValue;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) {
        *value += step;
        if (*value > maxValue) *value = maxValue;
        changed = true;
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
    return changed;
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

    if (!ImGui::Begin(
            "Vehicle LED System",
            &g_editorOpen
        )) {
        ImGui::End();
        return;
    }

    ImGui::Text("Vehicle LED System");
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

        ImGui::End();
        return;
    }

    ImGui::Text(
        "Vehicle Model ID: %d",
        modelId
    );

    VLS::VehicleProfile* profile =
        EnsureEditorProfile(modelId);

    if (profile == nullptr ||
        profile->placements.empty()) {
        ImGui::Text("Profile unavailable.");
        ImGui::End();
        return;
    }

    if (g_selectedPlacement < 0)
        g_selectedPlacement = 0;
    if (g_selectedPlacement >= static_cast<int>(profile->placements.size()))
        g_selectedPlacement = static_cast<int>(profile->placements.size()) - 1;

    if (ImGui::Button("Add LED / Light", ImVec2(180.0f, 38.0f))) {
        VLS::LedPlacement added;
        uint32_t nextId = 1;
        for (const VLS::LedPlacement& existing : profile->placements) {
            if (existing.id >= nextId)
                nextId = existing.id + 1;
        }
        added.id = nextId;
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
    ImGui::Text("LEDs: %d", static_cast<int>(profile->placements.size()));
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

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

    if (placement.effects.empty()) {
        ImGui::Text("No LED effect available.");
        ImGui::End();
        return;
    }

    VLS::LedEffectConfig& effect =
        placement.effects[0];

    bool changed = false;

    // -----------------------------------------------------
    // Animation Controller
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("Animation Controller");

    int modeIndex =
        GetAnimationModeIndex(g_animationMode);

    if (ImGui::Combo(
            "Animation Mode",
            &modeIndex,
            g_animationModeNames,
            5
        )) {
        SetAnimationMode(
            g_animationModeTypes[modeIndex]
        );
    }

    int interval = static_cast<int>(
        g_animationIntervalMs
    );

    if (ImGui::SliderInt(
            "Switch Interval (ms)",
            &interval,
            100,
            30000
        )) {
        g_animationIntervalMs =
            static_cast<uint32_t>(interval);

        g_animationController.SetIntervalMs(
            g_animationIntervalMs
        );
    }

    if (g_animationMode ==
        VLS::AnimationMode::Custom) {
        ImGui::Text(
            "Custom mode uses the profile sequence."
        );
    } else if (g_animationMode ==
               VLS::AnimationMode::Manual) {
        ImGui::Text(
            "Manual mode uses the selected editor effect."
        );
    } else {
        ImGui::Text(
            "Controller selects compatible enabled effects."
        );
    }

    // -----------------------------------------------------
    // Manual selection
    // -----------------------------------------------------

    g_animationController.SetManualSelection(
        placement.id,
        0
    );

    // -----------------------------------------------------
    // LED Zone
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Zone");

    int zoneIndex = GetZoneIndex(placement.zone);

    if (ImGui::Combo(
            "Placement Zone",
            &zoneIndex,
            g_zoneNames,
            5
        )) {
        placement.zone = g_zoneTypes[zoneIndex];
        changed = true;
    }

    // -----------------------------------------------------
    // Placement
    // -----------------------------------------------------

    ImGui::Separator();
    ImGui::Text("LED Placement");

    ImGui::Text("Position (fine adjustment: << / >>)");
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
    changed |= DrawSpacedFloatControl("Stretch X", &placement.transform.stretchX, 0.1f, 4.0f, 0.01f);
    changed |= DrawSpacedFloatControl("Stretch Y", &placement.transform.stretchY, 0.1f, 4.0f, 0.01f);

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

    if (ImGui::Button(
            "Reset Position",
            ImVec2(160.0f, 40.0f)
        )) {
        placement.transform.x = 0.0f;
        placement.transform.y = 0.0f;
        placement.transform.z = 0.0f;

        g_profileManager.MarkDirty(modelId);
    }

    ImGui::Spacing();

    ImGui::Text(
        "Changes are applied immediately."
    );

    ImGui::Text(
        "Close this window without disabling LED."
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

static void OnRenderEffects() {

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

                if (g_animationMode ==
                    VLS::AnimationMode::Custom) {

                    const VLS::EffectSequenceStep* step =
                        g_sequenceRuntime.GetCurrentStep(
                            *profile
                        );

                    if (step != nullptr) {
                        VLS::ProfileRenderer::RenderSelectedEffect(
                            vehicle,
                            *profile,
                            step->placementId,
                            step->effectIndex
                        );
                    }

                } else {
                    const VLS::AnimationSelection& selection =
                        g_animationController.GetSelection();

                    if (selection.valid) {
                        VLS::ProfileRenderer::RenderSelectedEffect(
                            vehicle,
                            *profile,
                            selection.placementId,
                            selection.effectIndex
                        );
                    }
                }
            }
        }
    }

    g_frameVehicles.clear();
    g_frameVehicleSet.clear();
}


// ---------------------------------------------------------
// Vehicle geometry scan
// ---------------------------------------------------------

static void ResetVehicleGeometryScan() {
    // Lepaskan mesh kendaraan sebelumnya.
    VLS::VehicleGeometryReader::Clear();

    g_geometryScanVehicle = nullptr;
    g_geometryScanModelId = -1;
    g_nextGeometryScanTimeMs = 0;
    g_geometryScanSucceeded = false;
}

static void UpdateVehicleGeometryScan() {
    CVehicle* vehicle =
        g_vehicleManager.GetCurrentVehicle();

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (vehicle == nullptr ||
        !IsVehiclePointerValid(vehicle) ||
        modelId < 0) {

        ResetVehicleGeometryScan();
        return;
    }

    // Kendaraan atau model berubah: lepaskan cache lama
    // sebelum memulai pemindaian kendaraan yang baru.
    if (vehicle != g_geometryScanVehicle ||
        modelId != g_geometryScanModelId) {

        VLS::VehicleGeometryReader::Clear();

        g_geometryScanVehicle = vehicle;
        g_geometryScanModelId = modelId;
        g_geometryScanSucceeded = false;
        g_nextGeometryScanTimeMs = 0;
    }

    if (g_geometryScanSucceeded)
        return;

    const uint32_t nowMs = CTimer::GetTimeMS();

    // Perbandingan ini aman terhadap rollover timer uint32_t.
    if (static_cast<int32_t>(
            nowMs - g_nextGeometryScanTimeMs
        ) < 0) {
        return;
    }

    // Jika clump belum tersedia, coba lagi setelah 2 detik.
    g_nextGeometryScanTimeMs = nowMs + 2000;

    VLS::GeometryScanStats stats;

    g_geometryScanSucceeded =
        VLS::VehicleGeometryReader::Scan(
            vehicle,
            modelId,
            stats
        );
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

    // Scan geometri kendaraan pemain.
    UpdateVehicleGeometryScan();

    ProcessPendingProfileLoads();

    const int modelId =
        g_vehicleManager.GetCurrentModelId();

    if (modelId != g_lastLoggedModelId) {
        g_lastLoggedModelId = modelId;

        ResetAnimationRuntime();

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

    UpdateAnimationController();
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
