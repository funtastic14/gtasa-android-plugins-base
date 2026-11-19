
#pragma once

#include "vehicle_profile_manager.h"

#include <cstdint>

namespace VLS {

class ProfileSequenceManager {
public:
    ProfileSequenceManager(
        VehicleProfileManager& profileManager,
        int modelId
    );

    int AddSequence();
    bool RemoveSequence(uint32_t sequenceIndex);
    const EffectSequence* FindSequence(uint32_t sequenceIndex) const;

    bool SetSequenceEnabled(
        uint32_t sequenceIndex,
        bool enabled
    );

    bool SetSequenceLoop(
        uint32_t sequenceIndex,
        bool loop
    );

    int AddStep(
        uint32_t sequenceIndex,
        uint32_t placementId,
        uint32_t effectIndex,
        uint32_t durationMs
    );

    bool RemoveStep(
        uint32_t sequenceIndex,
        uint32_t stepIndex
    );

    bool SetStepDuration(
        uint32_t sequenceIndex,
        uint32_t stepIndex,
        uint32_t durationMs
    );

private:
    VehicleProfileManager& m_profileManager;
    int m_modelId;

    VehicleProfile* GetEditableProfile();
    const VehicleProfile* GetReadableProfile() const;
};

} // namespace VLS
