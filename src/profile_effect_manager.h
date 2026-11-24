
#pragma once

#include "vehicle_profile_manager.h"

#include <cstdint>

namespace VLS {

class ProfileEffectManager {
public:
    ProfileEffectManager(
        VehicleProfileManager& profileManager,
        int modelId
    );

    // Menambahkan efek ke placement.
    // Mengembalikan indeks efek atau -1 jika gagal.
    int AddEffect(
        uint32_t placementId,
        LedEffectType type = LedEffectType::RunningLED
    );

    // Menghapus efek berdasarkan indeks.
    bool RemoveEffect(
        uint32_t placementId,
        uint32_t effectIndex
    );

    // Mengambil efek tanpa mengubahnya.
    const LedEffectConfig* FindEffect(
        uint32_t placementId,
        uint32_t effectIndex
    ) const;

    // Mengaktifkan atau menonaktifkan efek.
    bool SetEffectEnabled(
        uint32_t placementId,
        uint32_t effectIndex,
        bool enabled
    );

    // Mengatur jenis efek.
    bool SetEffectType(
        uint32_t placementId,
        uint32_t effectIndex,
        LedEffectType type
    );

    // Mengatur warna RGB.
    bool SetEffectColor(
        uint32_t placementId,
        uint32_t effectIndex,
        uint8_t red,
        uint8_t green,
        uint8_t blue
    );

    // Mengatur brightness dan kecepatan efek.
    bool SetEffectParameters(
        uint32_t placementId,
        uint32_t effectIndex,
        float brightness,
        float speed
    );

private:
    VehicleProfileManager& m_profileManager;
    int m_modelId;

    VehicleProfile* GetEditableProfile();

    const VehicleProfile* GetReadableProfile() const;
};

} // namespace VLS
