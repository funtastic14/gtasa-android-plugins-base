
#include "profile_sequence_runtime.h"

#include <cstddef>
#include <cstdint>

namespace VLS {
namespace {

bool IsStepReferenceValid(
    const VehicleProfile& profile,
    const EffectSequenceStep& step
) {
    if (step.placementId == 0 ||
        step.durationMs == 0) {
        return false;
    }

    for (const LedPlacement& placement :
         profile.placements) {
        if (placement.id != step.placementId) {
            continue;
        }

        return step.effectIndex < placement.effects.size();
    }

    return false;
}

bool IsSequenceValid(
    const EffectSequence& sequence
) {
    return sequence.enabled &&
           !sequence.steps.empty();
}

} // namespace

bool ProfileSequenceRuntime::Start(
    const VehicleProfile& profile,
    uint32_t sequenceIndex,
    uint32_t nowMs
) {
    Stop();

    if (sequenceIndex >= profile.sequences.size()) {
        return false;
    }

    const EffectSequence& sequence =
        profile.sequences[sequenceIndex];

    if (!IsSequenceValid(sequence)) {
        return false;
    }

    if (!IsStepReferenceValid(
            profile,
            sequence.steps.front()
        )) {
        return false;
    }

    m_sequenceIndex = sequenceIndex;
    m_stepIndex = 0;
    m_stepStartTimeMs = nowMs;

    m_running = true;
    m_finished = false;

    return true;
}

void ProfileSequenceRuntime::Stop() {
    m_running = false;
    m_finished = false;

    m_sequenceIndex = 0;
    m_stepIndex = 0;
    m_stepStartTimeMs = 0;
}

bool ProfileSequenceRuntime::Update(
    const VehicleProfile& profile,
    uint32_t nowMs
) {
    if (!m_running) {
        return false;
    }

    if (m_sequenceIndex >= profile.sequences.size()) {
        Stop();
        return false;
    }

    const EffectSequence& sequence =
        profile.sequences[m_sequenceIndex];

    if (!IsSequenceValid(sequence)) {
        Stop();
        return false;
    }

    if (m_stepIndex >= sequence.steps.size()) {
        Stop();
        return false;
    }

    const EffectSequenceStep& currentStep =
        sequence.steps[m_stepIndex];

    if (!IsStepReferenceValid(profile, currentStep)) {
        Stop();
        return false;
    }

    const uint32_t elapsedMs =
        nowMs - m_stepStartTimeMs;

    if (elapsedMs < currentStep.durationMs) {
        return true;
    }

    if (m_stepIndex + 1 < sequence.steps.size()) {
        ++m_stepIndex;
        m_stepStartTimeMs = nowMs;

        return true;
    }

    if (sequence.loop) {
        m_stepIndex = 0;
        m_stepStartTimeMs = nowMs;

        return true;
    }

    m_running = false;
    m_finished = true;

    return false;
}

const EffectSequenceStep*
ProfileSequenceRuntime::GetCurrentStep(
    const VehicleProfile& profile
) const {
    if (!m_running) {
        return nullptr;
    }

    if (m_sequenceIndex >= profile.sequences.size()) {
        return nullptr;
    }

    const EffectSequence& sequence =
        profile.sequences[m_sequenceIndex];

    if (m_stepIndex >= sequence.steps.size()) {
        return nullptr;
    }

    const EffectSequenceStep& step =
        sequence.steps[m_stepIndex];

    if (!IsStepReferenceValid(profile, step)) {
        return nullptr;
    }

    return &step;
}

bool ProfileSequenceRuntime::IsRunning() const {
    return m_running;
}

bool ProfileSequenceRuntime::IsFinished() const {
    return m_finished;
}

uint32_t ProfileSequenceRuntime::GetSequenceIndex() const {
    return m_sequenceIndex;
}

uint32_t ProfileSequenceRuntime::GetStepIndex() const {
    return m_stepIndex;
}

} // namespace VLS
