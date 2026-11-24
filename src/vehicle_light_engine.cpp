#include "vehicle_light_engine.h"

#include <algorithm>

#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/game_sa/engine/PointLights.h>
#include <plugin-sdk/game_sa/entity/Vehicle.h>

namespace VLS {

void VehicleLightEngine::AddHighBeam(
    CVehicle* vehicle,
    const VehicleProfile& profile
) {
    if (vehicle == nullptr || !profile.enabled ||
        !profile.nativeLights.highBeamEnabled) {
        return;
    }

    CMatrix* matrix = vehicle->GetMatrix();
    if (matrix == nullptr)
        return;

    const float range = std::max(
        10.0f,
        std::min(100.0f, profile.nativeLights.highBeamRange)
    );
    const float intensity = std::max(
        0.1f,
        std::min(4.0f, profile.nativeLights.highBeamIntensity)
    );

    // CMatrix::up is the vehicle forward axis in GTA SA.
    // Keep the two sources slightly apart to approximate left/right lamps.
    const CVector forward = matrix->up;
    const CVector right = matrix->right;
    const CVector base = matrix->pos + forward * 1.8f + matrix->at * 0.45f;

    for (int side = -1; side <= 1; side += 2) {
        const CVector position = base + right * (0.55f * side);
        CPointLights::AddLight(
            PLTYPE_SPOTLIGHT,
            position,
            forward,
            range,
            intensity,
            intensity,
            intensity,
            FOGEFF_ON,
            false,
            vehicle
        );
    }
}

} // namespace VLS
