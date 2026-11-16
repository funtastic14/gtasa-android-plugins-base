
#pragma once

#include <game_sa/entity/Vehicle.h>

namespace VLS {

class VehicleManager {
public:
    void Update();

    bool HasCurrentVehicle() const;
    CVehicle* GetCurrentVehicle() const;
    int GetCurrentModelId() const;

private:
    CVehicle* m_currentVehicle = nullptr;
    int m_currentModelId = -1;
};

}
