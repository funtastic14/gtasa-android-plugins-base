
#pragma once

#include <cstdint>

#include <plugin-sdk/game_sa/entity/Vehicle.h>

#include "vehicle_profile.h"

namespace VLS {

class ProfileRenderer {
public:
    static uint32_t RenderVehicleProfile(
        CVehicle* vehicle,
        const VehicleProfile& profile
    );

    static bool RenderSelectedEffect(
        CVehicle* vehicle,
        const VehicleProfile& profile,
        uint32_t placementId,
        uint32_t effectIndex
    );
};

}
