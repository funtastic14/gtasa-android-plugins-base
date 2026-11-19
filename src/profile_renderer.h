
#pragma once

#include <cstdint>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include "vehicle_profile.h"

namespace VLS {

class ProfileRenderer {
public:
    // Render static RunningLED placements for one vehicle.
    // Must be called during the vehicle render event.
    static uint32_t RenderVehicleProfile(
        CVehicle* vehicle,
        const VehicleProfile& profile
    );
};

} // namespace VLS
