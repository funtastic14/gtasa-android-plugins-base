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
        std::min(250.0f, profile.nativeLights.highBeamRange)
    );
    const float intensity = std::max(
        0.1f,
        std::min(4.0f, profile.nativeLights.highBeamIntensity)
    );

    // GTA vehicle matrix basis: right = lateral, up = vertical, at = forward.
    // The previous implementation used 'up' as forward, aiming the spotlights
    // toward the sky instead of along the road.
    const CVector forward = matrix->at;
    const CVector right = matrix->right;
    const CVector vertical = matrix->up;
    const CVector base = matrix->pos + forward * 1.8f + vertical * 0.45f;

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
