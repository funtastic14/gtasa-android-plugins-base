
#pragma once

#include <cstdint>

#include "vehicle_profile.h"

namespace VLS {

enum class AnimationMode : uint8_t {
    Manual,
    Automatic,
    Random,
    Sequential,
    Custom
};

struct AnimationSelection {
    bool valid = false;
    uint32_t placementId = 0;
    uint32_t effectIndex = 0;
    LedEffectType type = LedEffectType::RunningLED;
};

class AnimationController {
public:
    void SetMode(AnimationMode mode);
    AnimationMode GetMode() const;

    void SetIntervalMs(uint32_t intervalMs);
    uint32_t GetIntervalMs() const;

    void SetManualSelection(
        uint32_t placementId,
        uint32_t effectIndex
    );

    void SetRandomSeed(uint32_t seed);

    // Returns true when a valid effect is selected.
    bool Update(
        const VehicleProfile& profile,
        uint32_t nowMs
    );

    const AnimationSelection& GetSelection() const;

    void Reset();

private:
    AnimationMode m_mode = AnimationMode::Manual;

    uint32_t m_intervalMs = 3000;
    uint32_t m_lastChangeMs = 0;
    uint32_t m_randomState = 0xA341316Cu;

    uint32_t m_manualPlacementId = 0;
    uint32_t m_manualEffectIndex = 0;

    uint32_t m_currentCandidate = 0;

    bool m_hasSelection = false;

    AnimationSelection m_selection;
};

} // namespace VLS
