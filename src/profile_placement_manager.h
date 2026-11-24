
#pragma once

#include "vehicle_profile_manager.h"

#include <cstdint>

namespace VLS {

class ProfilePlacementManager {
public:
    ProfilePlacementManager(
        VehicleProfileManager& profileManager,
        int modelId
    );

    // Menambahkan placement baru.
    // Mengembalikan ID placement, atau 0 jika gagal.
    uint32_t AddPlacement(
        LedZone zone = LedZone::ExteriorBody
    );

    // Menghapus placement berdasarkan ID.
    bool RemovePlacement(uint32_t placementId);

    // Mengambil placement berdasarkan ID tanpa mengubahnya.
    const LedPlacement* FindPlacement(
        uint32_t placementId
    ) const;

    // Mengubah zona placement.
    bool SetZone(
        uint32_t placementId,
        LedZone zone
    );

    // Mengubah posisi, rotasi, lebar, dan tinggi placement.
    bool SetTransform(
        uint32_t placementId,
        const LedTransform& transform
    );

private:
    VehicleProfileManager& m_profileManager;
    int m_modelId;

    // Mengambil profil yang sedang dikelola.
    VehicleProfile* GetEditableProfile();

    // Mengambil profil yang tersedia tanpa mengubahnya.
    const VehicleProfile* GetReadableProfile() const;
};

} // namespace VLS
