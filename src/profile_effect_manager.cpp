
#include "profile_effect_manager.h"

#include <cmath>
#include <limits>

namespace VLS {

ProfileEffectManager::ProfileEffectManager(
    VehicleProfileManager& profileManager,
    int modelId
)
    : m_profileManager(profileManager),
      m_modelId(modelId) {
}

VehicleProfile* ProfileEffectManager::GetEditableProfile() {
    if (m_modelId < 0) {
        return m_profileManager.GetEditableDefault();
    }

    return m_profileManager.GetEditable(m_modelId);
}

const VehicleProfile*
ProfileEffectManager::GetReadableProfile() const {
    if (m_modelId < 0) {
        return &m_profileManager.GetProfileOrDefault(-1);
    }

    return m_profileManager.Find(m_modelId);
}

int ProfileEffectManager::AddEffect(
    uint32_t placementId,
    LedEffectType type
) {
    if (placementId == 0) {
        return -1;
    }

    const VehicleProfile* readableProfile =
        GetReadableProfile();

    if (readableProfile == nullptr) {
        return -1;
    }

    bool placementExists = false;

    for (const LedPlacement& placement :
         readableProfile->placements) {
        if (placement.id == placementId) {
            placementExists = true;
            break;
        }
    }

    if (!placementExists) {
        return -1;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return -1;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (placement.effects.size() >=
            static_cast<size_t>(std::numeric_limits<int>::max())) {
            return -1;
        }

        LedEffectConfig effect;
        effect.type = type;

        const int effectIndex =
            static_cast<int>(placement.effects.size());

        placement.effects.push_back(effect);

        return effectIndex;
    }

    return -1;
}

bool ProfileEffectManager::RemoveEffect(
    uint32_t placementId,
    uint32_t effectIndex
) {
    if (placementId == 0) {
        return false;
    }

    const VehicleProfile* readableProfile =
        GetReadableProfile();

    if (readableProfile == nullptr) {
        return false;
    }

    bool effectExists = false;

    for (const LedPlacement& placement :
         readableProfile->placements) {
        if (placement.id == placementId &&
            effectIndex < placement.effects.size()) {
            effectExists = true;
            break;
        }
    }

    if (!effectExists) {
        return false;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return false;
        }

        placement.effects.erase(
            placement.effects.begin() + effectIndex
        );

        return true;
    }

    return false;
}

const LedEffectConfig* ProfileEffectManager::FindEffect(
    uint32_t placementId,
    uint32_t effectIndex
) const {
    if (placementId == 0) {
        return nullptr;
    }

    const VehicleProfile* profile = GetReadableProfile();

    if (profile == nullptr) {
        return nullptr;
    }

    for (const LedPlacement& placement :
         profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return nullptr;
        }

        return &placement.effects[effectIndex];
    }

    return nullptr;
}

bool ProfileEffectManager::SetEffectEnabled(
    uint32_t placementId,
    uint32_t effectIndex,
    bool enabled
) {
    const LedEffectConfig* current =
        FindEffect(placementId, effectIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->enabled == enabled) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return false;
        }

        placement.effects[effectIndex].enabled = enabled;

        return true;
    }

    return false;
}

bool ProfileEffectManager::SetEffectType(
    uint32_t placementId,
    uint32_t effectIndex,
    LedEffectType type
) {
    const uint8_t typeValue = static_cast<uint8_t>(type);

    if (typeValue >
        static_cast<uint8_t>(LedEffectType::CustomAnimation)) {
        return false;
    }

    const LedEffectConfig* current =
        FindEffect(placementId, effectIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->type == type) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return false;
        }

        placement.effects[effectIndex].type = type;

        return true;
    }

    return false;
}

bool ProfileEffectManager::SetEffectColor(
    uint32_t placementId,
    uint32_t effectIndex,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    const LedEffectConfig* current =
        FindEffect(placementId, effectIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->red == red &&
        current->green == green &&
        current->blue == blue) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return false;
        }

        LedEffectConfig& effect =
            placement.effects[effectIndex];

        effect.red = red;
        effect.green = green;
        effect.blue = blue;

        return true;
    }

    return false;
}

bool ProfileEffectManager::SetEffectParameters(
    uint32_t placementId,
    uint32_t effectIndex,
    float brightness,
    float speed
) {
    if (!std::isfinite(brightness) ||
        !std::isfinite(speed)) {
        return false;
    }

    if (brightness < 0.0f ||
        brightness > 1.0f ||
        speed <= 0.0f) {
        return false;
    }

    const LedEffectConfig* current =
        FindEffect(placementId, effectIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->brightness == brightness &&
        current->speed == speed) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return false;
    }

    for (LedPlacement& placement : profile->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex >= placement.effects.size()) {
            return false;
        }

        LedEffectConfig& effect =
            placement.effects[effectIndex];

        effect.brightness = brightness;
        effect.speed = speed;

        return true;
    }

    return false;
}

} // namespace VLS
