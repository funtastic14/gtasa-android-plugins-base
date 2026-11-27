#include "vehicle_light_engine.h"

#include <algorithm>
#include <cmath>

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
        std::min(500.0f, profile.nativeLights.highBeamRange)
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

    const float widthDegrees = std::max(
        5.0f, std::min(45.0f, profile.nativeLights.highBeamWidthDegrees));
    const float halfSpread = std::tan(widthDegrees * 0.00872664625997f);
    // Three rays per lamp make width changes visible while retaining a
    // center beam. Six lights are still a bounded, allocation-free update.
    const float rayOffsets[3] = {-halfSpread, 0.0f, halfSpread};
    const float rayEnergy = intensity * 0.65f;
    // GTA SA keeps a small fixed point-light pool. Avoid overrunning it when
    // several profile-enabled vehicles request high beams in the same frame.
    if (CPointLights::NumLights >= 26)
        return;
    for (int side = -1; side <= 1; side += 2) {
        const CVector position = base + right * (0.55f * side);
        for (int ray = 0; ray < 3; ++ray) {
            CVector direction = forward + right * rayOffsets[ray];
            direction.NormaliseAndMag();
            CPointLights::AddLight(
                PLTYPE_SPOTLIGHT,
                position,
                direction,
                range,
                rayEnergy,
                rayEnergy,
                rayEnergy,
                FOGEFF_ON,
                false,
                vehicle
            );
        }
    }
}

} // namespace VLS
