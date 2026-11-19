#include "profile_sequence_runtime.h"

#include <cstddef>
#include <cstdint>

namespace VLS {
namespace {

bool IsStepReferenceValid(
    const VehicleProfile& profile,
    const EffectSequenceStep& step
) {
    if (step.placementId == 0 || step.durationMs == 0) {
        return false;
    }

    for (const LedPlacement& placement : profile.placements) {
        if (placement.id != step.placementId) {
            continue;
        }

        return step.effectIndex < placement.effects.size();
    }

    return false;
}

bool IsSequenceValid(const EffectSequence& sequence) {
    if (!sequence.enabled || sequence.steps.empty()) {
        return false;
    }

    return true;
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

    for (const EffectSequenceStep& step : sequence.steps) {
        if (!IsStepReferenceValid(profile, step)) {
            return false;
        }
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

    if (!IsSequenceValid(sequence) ||
        m_stepIndex >= sequence.steps.size()) {
        Stop();
        return false;
    }

    for (const EffectSequenceStep& step : sequence.steps) {
        if (!IsStepReferenceValid(profile, step)) {
            Stop();
            return false;
        }
    }

    // Pengurangan unsigned tetap aman ketika timer melewati UINT32_MAX.
    uint32_t elapsedMs = nowMs - m_stepStartTimeMs;

    if (sequence.loop) {
        uint64_t cycleDurationMs = 0;

        for (const EffectSequenceStep& step : sequence.steps) {
            cycleDurationMs += step.durationMs;
        }

        if (cycleDurationMs == 0) {
            Stop();
            return false;
        }

        if (static_cast<uint64_t>(elapsedMs) >= cycleDurationMs) {
            elapsedMs = static_cast<uint32_t>(
                static_cast<uint64_t>(elapsedMs) % cycleDurationMs
            );

            m_stepStartTimeMs = nowMs - elapsedMs;
        }
    }

    while (m_running) {
        const EffectSequenceStep& currentStep =
            sequence.steps[m_stepIndex];

        if (elapsedMs < currentStep.durationMs) {
            break;
        }

        elapsedMs -= currentStep.durationMs;
        m_stepStartTimeMs += currentStep.durationMs;

        if (m_stepIndex + 1 < sequence.steps.size()) {
            ++m_stepIndex;
            continue;
        }

        if (sequence.loop) {
            m_stepIndex = 0;
            continue;
        }

        // Sequence non-loop selesai setelah langkah terakhir berakhir.
        m_running = false;
        m_finished = true;
        return false;
    }

    return m_running;
}

const EffectSequenceStep* ProfileSequenceRuntime::GetCurrentStep(
    const VehicleProfile& profile
) const {
    if (!m_running ||
        m_sequenceIndex >= profile.sequences.size()) {
        return nullptr;
    }

    const EffectSequence& sequence =
        profile.sequences[m_sequenceIndex];

    if (m_stepIndex >= sequence.steps.size()) {
        return nullptr;
    }

    const EffectSequenceStep& step = sequence.steps[m_stepIndex];

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
