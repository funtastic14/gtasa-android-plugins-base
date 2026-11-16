
#pragma once

#include "vehicle_profile.h"

#include <unordered_map>

namespace VLS {

class VehicleProfileManager {
public:
    // Membuat atau mengambil profil kendaraan.
    VehicleProfile& GetOrCreate(int modelId);

    // Mengambil profil kendaraan jika tersedia.
    VehicleProfile* Find(int modelId);

    const VehicleProfile* Find(int modelId) const;

    // Mengambil profil kendaraan atau profil default.
    const VehicleProfile& GetProfileOrDefault(int modelId) const;

    // Mengatur profil default.
    void SetDefaultProfile(const VehicleProfile& profile);

    // Menghapus profil kendaraan.
    bool Remove(int modelId);

    // Menghapus seluruh profil kendaraan.
    void Clear();

private:
    std::unordered_map<int, VehicleProfile> m_profiles;

    VehicleProfile m_defaultProfile;
};

} // namespace VLS
