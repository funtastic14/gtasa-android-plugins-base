
#pragma once

#include <string>

#include "vehicle_profile.h"

namespace VLS {
namespace ProfileJson {

bool Serialize(const VehicleProfile& profile, std::string& json);

bool Deserialize(const std::string& json, VehicleProfile& profile);

}
}
