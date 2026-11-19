
#include "profile_placement_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace VLS {

ProfilePlacementManager::ProfilePlacementManager(
    VehicleProfileManager& profileManager,
    int modelId
)
    : m_profileManager(profileManager),
      m_modelId(modelId) {
}

VehicleProfile* ProfilePlacementManager::GetEditableProfile() {
    if (m_modelId < 0) {
        return m_profileManager.GetEditableDefault();
    }

    return m_profileManager.GetEditable(m_modelId);
}

const VehicleProfile*
ProfilePlacementManager::GetReadableProfile() const {
    if (m_modelId < 0) {
        return &m_profileManager.GetProfileOrDefault(-1);
    }

    return m_profileManager.Find(m_modelId);
}

uint32_t ProfilePlacementManager::AddPlacement(LedZone zone) {
    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return 0;
    }

    // Cari ID unik mulai dari 1.
    uint32_t newId = 1;

    while (true) {
        const bool idExists = std::any_of(
            profile->placements.begin(),
            profile->placements.end(),
            [newId](const LedPlacement& placement) {
                return placement.id == newId;
            }
        );

        if (!idExists) {
            break;
        }

        if (newId == std::numeric_limits<uint32_t>::max()) {
            return 0;
        }

        ++newId;
    }

    LedPlacement placement;
    placement.id = newId;
    placement.zone = zone;

    profile->placements.push_back(placement);

    return newId;
}

bool ProfilePlacementManager::RemovePlacement(
    uint32_t placementId
) {
    if (placementId == 0) {
        return false;
    }

    const VehicleProfile* readableProfile =
        GetReadableProfile();

    if (readableProfile == nullptr) {
        return false;
    }

    const auto readableIt = std::find_if(
        readableProfile->placements.begin(),
        readableProfile->placements.end(),
        [placementId](const LedPlacement& placement) {
            return placement.id == placementId;
        }
    );

    if (readableIt == readableProfile->placements.end()) {
        return false;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    const auto it = std::find_if(
        profile->placements.begin(),
        profile->placements.end(),
        [placementId](const LedPlacement& placement) {
            return placement.id == placementId;
        }
    );

    if (it == profile->placements.end()) {
        return false;
    }

    // Hapus placement.
    profile->placements.erase(it);

    // Hapus seluruh step sequence yang menggunakan placement ini.
    for (EffectSequence& sequence : profile->sequences) {
        auto& steps = sequence.steps;

        steps.erase(
            std::remove_if(
                steps.begin(),
                steps.end(),
                [placementId](const EffectSequenceStep& step) {
                    return step.placementId == placementId;
                }
            ),
            steps.end()
        );
    }

    return true;
}

const LedPlacement* ProfilePlacementManager::FindPlacement(
    uint32_t placementId
) const {
    if (placementId == 0) {
        return nullptr;
    }

    const VehicleProfile* profile = GetReadableProfile();

    if (profile == nullptr) {
        return nullptr;
    }

    const auto it = std::find_if(
        profile->placements.begin(),
        profile->placements.end(),
        [placementId](const LedPlacement& placement) {
            return placement.id == placementId;
        }
    );

    if (it == profile->placements.end()) {
        return nullptr;
    }

    return &(*it);
}

bool ProfilePlacementManager::SetZone(
    uint32_t placementId,
    LedZone zone
) {
    if (placementId == 0) {
        return false;
    }

    const LedPlacement* current =
        FindPlacement(placementId);

    if (current == nullptr) {
        return false;
    }

    if (current->zone == zone) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    const auto it = std::find_if(
        profile->placements.begin(),
        profile->placements.end(),
        [placementId](const LedPlacement& placement) {
            return placement.id == placementId;
        }
    );

    if (it == profile->placements.end()) {
        return false;
    }

    it->zone = zone;

    return true;
}

bool ProfilePlacementManager::SetTransform(
    uint32_t placementId,
    const LedTransform& transform
) {
    if (placementId == 0) {
        return false;
    }

    // Pastikan seluruh nilai transformasi valid.
    if (!std::isfinite(transform.x) ||
        !std::isfinite(transform.y) ||
        !std::isfinite(transform.z) ||
        !std::isfinite(transform.rotationX) ||
        !std::isfinite(transform.rotationY) ||
        !std::isfinite(transform.rotationZ) ||
        !std::isfinite(transform.width) ||
        !std::isfinite(transform.height)) {
        return false;
    }

    if (transform.width <= 0.0f ||
        transform.height <= 0.0f) {
        return false;
    }

    if (FindPlacement(placementId) == nullptr) {
        return false;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    const auto it = std::find_if(
        profile->placements.begin(),
        profile->placements.end(),
        [placementId](const LedPlacement& placement) {
            return placement.id == placementId;
        }
    );

    if (it == profile->placements.end()) {
        return false;
    }

    it->transform = transform;

    return true;
}

} // namespace VLS
