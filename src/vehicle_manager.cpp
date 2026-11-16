
#include "vehicle_manager.h"

#include <plugin-sdk/game_sa/PlayerPed.h>

namespace VLS {

void VehicleManager::Update() {
    CVehicle* vehicle = FindPlayerVehicle(0, false);

    if (vehicle == nullptr || !IsVehiclePointerValid(vehicle)) {
        m_currentVehicle = nullptr;
        m_currentModelId = -1;
        return;
    }

    m_currentVehicle = vehicle;
    m_currentModelId = vehicle->m_nModelIndex;
}

bool VehicleManager::HasCurrentVehicle() const {
    return m_currentVehicle != nullptr;
}

CVehicle* VehicleManager::GetCurrentVehicle() const {
    return m_currentVehicle;
}

int VehicleManager::GetCurrentModelId() const {
    return m_currentModelId;
}

}
