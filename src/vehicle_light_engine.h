#pragma once

#include "vehicle_profile.h"

struct CVehicle;

namespace VLS {
class VehicleLightEngine {
public:
    static void AddHighBeam(CVehicle* vehicle, const VehicleProfile& profile);
};
}
