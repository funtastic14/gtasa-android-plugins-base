
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
}

bool VehicleProfileManager::Load(int modelId) {
    std::string json;

    if (!ProfileStorage::LoadProfileJson(modelId, json)) {
        return false;
    }

    VehicleProfile profile;

    if (!ProfileJson::Deserialize(json, profile)) {
        return false;
    }

    if (profile.modelId != modelId) {
        return false;
    }

    if (modelId < 0) {
        m_defaultProfile = profile;
        m_defaultProfile.modelId = -1;
    } else {
        m_profiles[modelId] = profile;
    }

    return true;
}

bool VehicleProfileManager::Save(int modelId) const {
    const VehicleProfile* profile = nullptr;

    if (modelId < 0) {
        profile = &m_defaultProfile;
    } else {
        profile = Find(modelId);
    }

    if (profile == nullptr) {
        return false;
    }

    VehicleProfile profileToSave = *profile;
    profileToSave.modelId = modelId;

    std::string json;

    if (!ProfileJson::Serialize(profileToSave, json)) {
        return false;
    }

    return ProfileStorage::SaveProfileJson(modelId, json);
}

bool VehicleProfileManager::Remove(int modelId) {
    return m_profiles.erase(modelId) > 0;
}

void VehicleProfileManager::Clear() {
    m_profiles.clear();
}

} // namespace VLS
