
#pragma once

#include <cstdint>

#include <plugin-sdk/game_sa/entity/Vehicle.h>

#include "vehicle_profile.h"

namespace VLS {

struct NativeLightRenderStats {
    uint32_t lightingPatches = 0;
    uint32_t frontPatches = 0;
    uint32_t rearPatches = 0;
    uint32_t unclassifiedSidePatches = 0;
    uint32_t sideDisabled = 0;
    uint32_t missingMesh = 0;
    uint32_t invalidGeometry = 0;
    uint32_t missingVehicleMatrix = 0;
    uint32_t emptyTriangleOutput = 0;
    uint32_t transformFailed = 0;
    uint32_t drawFailed = 0;
    uint32_t renderedPatches = 0;
    uint32_t submittedVertices = 0;
    uint32_t submittedIndices = 0;
    uint32_t validTriangles = 0;
    uint32_t alpha = 0;
};

class ProfileRenderer {
public:
    static uint32_t RenderVehicleProfile(
        CVehicle* vehicle,
        const VehicleProfile& profile,
        uint32_t frameTimeMs,
        const EffectSequenceStep* activeStep = nullptr
    );

    static bool RenderSelectedEffect(
        CVehicle* vehicle,
        const VehicleProfile& profile,
        uint32_t placementId,
        uint32_t effectIndex
    );

    static uint32_t RenderNativeLights(
        CVehicle* vehicle,
        const VehicleProfile& profile,
        uint32_t frameTimeMs,
        NativeLightRenderStats* stats = nullptr
    );
};

}
