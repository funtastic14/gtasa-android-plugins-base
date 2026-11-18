
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
    } else {
        m_profiles[storageModelId] = profile;
    }

    return true;
}

bool VehicleProfileManager::Save(int modelId) const {
    const int storageModelId = modelId < 0 ? -1 : modelId;

    const VehicleProfile* profile = nullptr;

    if (storageModelId < 0) {
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

    return ProfileStorage::SaveProfileJson(storageModelId, json);
}

bool VehicleProfileManager::SaveAll() const {
    bool success = true;

    // Simpan profil default hanya jika pernah dimuat atau diatur.
    if (m_hasDefaultProfile) {
        if (!Save(-1)) {
            success = false;
        }
    }

    // Simpan seluruh profil kendaraan yang ada di dalam manager.
    for (const auto& entry : m_profiles) {
        if (!Save(entry.first)) {
            success = false;
        }
    }

    return success;
}

bool VehicleProfileManager::Remove(int modelId) {
    return m_profiles.erase(modelId) > 0;
}

void VehicleProfileManager::Clear() {
    m_profiles.clear();
}

} // namespace VLS
