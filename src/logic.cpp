
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
static int g_editorPage = 0;
static int g_adjustmentScaleMode = 1;
static const char* g_adjustmentScaleNames[] = {"Very Fine", "Normal", "Fast", "Very Fast"};
static const float g_adjustmentScaleValues[] = {0.1f, 1.0f, 5.0f, 20.0f};
static char g_runningTextBuffer[129] = {};
static const VLS::LedEffectConfig* g_runningTextBufferEffect = nullptr;
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

    bool changed = false;

    if (g_selectedEffect < 0)
        g_selectedEffect = 0;
    if (g_selectedEffect >= static_cast<int>(placement.effects.size()))
        g_selectedEffect = placement.effects.empty() ? 0 : static_cast<int>(placement.effects.size()) - 1;

    if (ImGui::Button("Add Effect", ImVec2(125.0f, 38.0f))) {
        VLS::LedEffectConfig addedEffect;
        addedEffect.type = VLS::LedEffectType::RunningLED;
        addedEffect.red = 0;
        addedEffect.green = 180;
        addedEffect.blue = 255;
        placement.effects.push_back(addedEffect);
        g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Effect", ImVec2(125.0f, 38.0f)) && !placement.effects.empty()) {
        placement.effects.push_back(placement.effects[g_selectedEffect]);
        g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Effect", ImVec2(125.0f, 38.0f)) && placement.effects.size() > 1) {
        placement.effects.erase(placement.effects.begin() + g_selectedEffect);
        if (g_selectedEffect >= static_cast<int>(placement.effects.size()))
            g_selectedEffect = static_cast<int>(placement.effects.size()) - 1;
        changed = true;
    }

    if (placement.effects.empty()) {
        ImGui::TextWrapped("Add an effect to this LED placement to begin.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    ImGui::Text("Effects on this LED: %d", static_cast<int>(placement.effects.size()));
    ImGui::SliderInt("Selected Effect", &g_selectedEffect, 0, static_cast<int>(placement.effects.size()) - 1);
    VLS::LedEffectConfig& effect = placement.effects[g_selectedEffect];

    if (ImGui::Button("LED & EFFECT", ImVec2(190.0f, 36.0f)))
        g_editorPage = 0;
    ImGui::SameLine();
    if (ImGui::Button("POSITION", ImVec2(190.0f, 36.0f)))
        g_editorPage = 1;
    ImGui::Separator();

    if (g_editorPage == 0) {
    // -----------------------------------------------------
    // Per-placement animation controller.
    ImGui::Separator();
    ImGui::Text("Animation Controller - This LED Only");
    int modeIndex = static_cast<int>(placement.animationMode);
    if (ImGui::Combo("Animation Mode", &modeIndex, g_animationModeNames, 5)) {
        if (modeIndex < 0) modeIndex = 0;
        if (modeIndex > 4) modeIndex = 4;
        placement.animationMode = static_cast<uint8_t>(modeIndex);
        g_placementControllers[placement.id].Reset();
        changed = true;
    }
    int interval = static_cast<int>(placement.animationIntervalMs);
    if (ImGui::SliderInt("Pattern Switch Interval (ms)", &interval, 100, 30000)) {
        placement.animationIntervalMs = static_cast<uint32_t>(interval);
        g_placementControllers[placement.id].Reset();
        changed = true;
    }
    if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Manual))
        ImGui::TextWrapped("Manual: this LED keeps its first enabled effect.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Automatic))
        ImGui::TextWrapped("Automatic: this LED cycles its compatible patterns only.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Random))
        ImGui::TextWrapped("Random: this LED randomly selects its own enabled patterns.");
    else if (placement.animationMode == static_cast<uint8_t>(VLS::AnimationMode::Sequential))
        ImGui::TextWrapped("Sequential: this LED follows its own effect list.");
    else
        ImGui::TextWrapped("Custom: currently uses this LED's effect order; editable custom timeline is next.");

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
        ImGui::TextWrapped("Choose Pattern > Running Text, open the touch keyboard, type your message, then tap DONE and Save Profile.");
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
        const char* colorModeNames[] = {"Solid Color", "Two-Color Gradient", "Rainbow"};
        int colorMode = static_cast<int>(effect.colorMode);
        if (ImGui::Combo("Color Style", &colorMode, colorModeNames, 3)) {
            effect.colorMode = static_cast<VLS::LedColorMode>(colorMode);
            changed = true;
        }
        if (effect.colorMode == VLS::LedColorMode::Gradient) {
            float secondaryColor[3] = {
                effect.secondaryRed / 255.0f,
                effect.secondaryGreen / 255.0f,
                effect.secondaryBlue / 255.0f
            };
            if (ImGui::ColorEdit3("Gradient End Color", secondaryColor)) {
                effect.secondaryRed = static_cast<uint8_t>(secondaryColor[0] * 255.0f);
                effect.secondaryGreen = static_cast<uint8_t>(secondaryColor[1] * 255.0f);
                effect.secondaryBlue = static_cast<uint8_t>(secondaryColor[2] * 255.0f);
                changed = true;
            }
        }
        ImGui::TextWrapped("RGB remains a separate animated color mode.");
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

                for (const VLS::LedPlacement& led : profile->placements) {
                    if (!led.enabled)
                        continue;
                    const auto controllerIt = g_placementControllers.find(led.id);
                    if (controllerIt == g_placementControllers.end())
                        continue;
                    const VLS::AnimationSelection& selection = controllerIt->second.GetSelection();
                    if (selection.valid) {
                        VLS::ProfileRenderer::RenderSelectedEffect(
                            vehicle, *profile, selection.placementId, selection.effectIndex
                        );
                    }
                }    }
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

    // Automatic geometry scanning is paused until windshield work resumes.

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
