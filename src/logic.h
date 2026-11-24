
#pragma once

namespace VLS {
struct VehicleProfile;
}

namespace Logic {

void Init();
void Shutdown();

const VLS::VehicleProfile* GetCurrentProfile();

}
