
#pragma once

#include "vehicle_profile.h"

#include <unordered_map>
#include <unordered_set>

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

    // Menandai profil sebagai berubah.
    void MarkDirty(int modelId);

    // Memeriksa apakah profil mengalami perubahan.
    bool IsDirty(int modelId) const;

    // Memuat profil dari penyimpanan JSON.
    bool Load(int modelId);

    // Menyimpan satu profil.
    bool Save(int modelId);

    // Menyimpan seluruh profil yang berubah.
    bool SaveAll();

    // Menghapus profil kendaraan.
    bool Remove(int modelId);

    // Menghapus seluruh profil kendaraan.
    void Clear();

private:
    std::unordered_map<int, VehicleProfile> m_profiles;

    std::unordered_set<int> m_dirtyProfiles;

    VehicleProfile m_defaultProfile;

    bool m_hasDefaultProfile = false;
    bool m_defaultDirty = false;
};

} // namespace VLS
