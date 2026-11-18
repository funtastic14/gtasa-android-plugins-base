
#include "vehicle_profile_manager.h"

#include "profile_json.h"
#include "profile_storage.h"

#include <string>

namespace VLS {

VehicleProfile& VehicleProfileManager::GetOrCreate(int modelId) {
    auto result = m_profiles.emplace(modelId, VehicleProfile{});

    VehicleProfile& profile = result.first->second;

    if (result.second) {
        profile.modelId = modelId;
        MarkDirty(modelId);
    }

    return profile;
}

VehicleProfile* VehicleProfileManager::Find(int modelId) {
    auto it = m_profiles.find(modelId);

    if (it == m_profiles.end()) {
        return nullptr;
    }

    return &it->second;
}

const VehicleProfile* VehicleProfileManager::Find(int modelId) const {
    auto it = m_profiles.find(modelId);

    if (it == m_profiles.end()) {
        return nullptr;
    }

    return &it->second;
}

const VehicleProfile& VehicleProfileManager::GetProfileOrDefault(
    int modelId
) const {
    const VehicleProfile* profile = Find(modelId);

    if (profile != nullptr) {
        return *profile;
    }

    return m_defaultProfile;
}

void VehicleProfileManager::SetDefaultProfile(
    const VehicleProfile& profile
) {
    m_defaultProfile = profile;
    m_defaultProfile.modelId = -1;

    m_hasDefaultProfile = true;
    m_defaultDirty = true;
}

void VehicleProfileManager::MarkDirty(int modelId) {
    if (modelId < 0) {
        m_hasDefaultProfile = true;
        m_defaultDirty = true;
        return;
    }

    m_dirtyProfiles.insert(modelId);
}

bool VehicleProfileManager::IsDirty(int modelId) const {
    if (modelId < 0) {
        return m_defaultDirty;
    }

    return m_dirtyProfiles.find(modelId) != m_dirtyProfiles.end();
}

bool VehicleProfileManager::Load(int modelId) {
    const int storageModelId = modelId < 0 ? -1 : modelId;

    std::string json;

    if (!ProfileStorage::LoadProfileJson(storageModelId, json)) {
        return false;
    }

    VehicleProfile profile;

    if (!ProfileJson::Deserialize(json, profile)) {
        return false;
    }

    if (profile.modelId != storageModelId) {
        return false;
    }

    if (storageModelId < 0) {
        m_defaultProfile = profile;
        m_defaultProfile.modelId = -1;

        m_hasDefaultProfile = true;
        m_defaultDirty = false;
    } else {
        m_profiles[storageModelId] = profile;
        m_dirtyProfiles.erase(storageModelId);
    }

    return true;
}

bool VehicleProfileManager::Save(int modelId) {
    const int storageModelId = modelId < 0 ? -1 : modelId;

    const VehicleProfile* profile = nullptr;

    if (storageModelId < 0) {
        if (!m_hasDefaultProfile) {
            return false;
        }

        profile = &m_defaultProfile;
    } else {
        profile = Find(storageModelId);
    }

    if (profile == nullptr) {
        return false;
    }

    VehicleProfile profileToSave = *profile;
    profileToSave.modelId = storageModelId;

    std::string json;

    if (!ProfileJson::Serialize(profileToSave, json)) {
        return false;
    }

    if (!ProfileStorage::SaveProfileJson(storageModelId, json)) {
        return false;
    }

    // Hapus penanda perubahan hanya setelah penyimpanan berhasil.
    if (storageModelId < 0) {
        m_defaultDirty = false;
    } else {
        m_dirtyProfiles.erase(storageModelId);
    }

    return true;
}

bool VehicleProfileManager::SaveAll() {
    bool success = true;

    // Simpan profil default hanya jika berubah.
    if (m_defaultDirty) {
        if (!Save(-1)) {
            success = false;
        }
    }

    // Simpan hanya profil kendaraan yang berubah.
    for (const auto& entry : m_profiles) {
        const int modelId = entry.first;

        if (m_dirtyProfiles.find(modelId) == m_dirtyProfiles.end()) {
            continue;
        }

        if (!Save(modelId)) {
            success = false;
        }
    }

    return success;
}

bool VehicleProfileManager::Remove(int modelId) {
    m_dirtyProfiles.erase(modelId);

    return m_profiles.erase(modelId) > 0;
}

void VehicleProfileManager::Clear() {
    m_profiles.clear();
    m_dirtyProfiles.clear();
}

} // namespace VLS
