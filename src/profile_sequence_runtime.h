
#pragma once

#include "vehicle_profile.h"

#include <cstdint>

namespace VLS {

class ProfileSequenceRuntime {
public:
    bool Start(
        const VehicleProfile& profile,
        uint32_t sequenceIndex,
        uint32_t nowMs
    );

    void Stop();

    bool Update(
        const VehicleProfile& profile,
        uint32_t nowMs
    );

    const EffectSequenceStep* GetCurrentStep(
        const VehicleProfile& profile
    ) const;

    const EffectSequenceStep* GetPreviousStep(
        const VehicleProfile& profile,
        uint32_t nowMs
    ) const;

    float GetTransitionProgress(uint32_t nowMs) const;
    EffectTransitionMode GetTransitionMode(const VehicleProfile& profile) const;
    EffectTransitionDirection GetTransitionDirection(const VehicleProfile& profile) const;

    bool IsRunning() const;
    bool IsFinished() const;

    uint32_t GetSequenceIndex() const;
    uint32_t GetStepIndex() const;

private:
    bool m_running = false;
    bool m_finished = false;

    uint32_t m_sequenceIndex = 0;
    uint32_t m_stepIndex = 0;
    uint32_t m_stepStartTimeMs = 0;
    EffectSequenceStep m_previousStep{};
    bool m_hasPreviousStep = false;
    uint32_t m_transitionStartTimeMs = 0;
    static constexpr uint32_t TRANSITION_DURATION_MS = 250;
};

} // namespace VLS
