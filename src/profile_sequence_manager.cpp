
#include "profile_sequence_manager.h"

#include <cstddef>
#include <limits>

namespace VLS {

ProfileSequenceManager::ProfileSequenceManager(
    VehicleProfileManager& profileManager,
    int modelId
)
    : m_profileManager(profileManager),
      m_modelId(modelId) {
}

VehicleProfile* ProfileSequenceManager::GetEditableProfile() {
    if (m_modelId < 0) {
        return m_profileManager.GetEditableDefault();
    }

    return m_profileManager.GetEditable(m_modelId);
}

const VehicleProfile*
ProfileSequenceManager::GetReadableProfile() const {
    if (m_modelId < 0) {
        return &m_profileManager.GetProfileOrDefault(-1);
    }

    return m_profileManager.Find(m_modelId);
}

int ProfileSequenceManager::AddSequence() {
    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr) {
        return -1;
    }

    if (profile->sequences.size() >=
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        return -1;
    }

    EffectSequence sequence;
    sequence.enabled = false;
    sequence.loop = true;

    const int sequenceIndex =
        static_cast<int>(profile->sequences.size());

    profile->sequences.push_back(sequence);

    return sequenceIndex;
}

bool ProfileSequenceManager::RemoveSequence(
    uint32_t sequenceIndex
) {
    const VehicleProfile* readable =
        GetReadableProfile();

    if (readable == nullptr ||
        sequenceIndex >= readable->sequences.size()) {
        return false;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return false;
    }

    profile->sequences.erase(
        profile->sequences.begin() + sequenceIndex
    );

    return true;
}

const EffectSequence* ProfileSequenceManager::FindSequence(
    uint32_t sequenceIndex
) const {
    const VehicleProfile* profile =
        GetReadableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return nullptr;
    }

    return &profile->sequences[sequenceIndex];
}

bool ProfileSequenceManager::SetSequenceEnabled(
    uint32_t sequenceIndex,
    bool enabled
) {
    const EffectSequence* current =
        FindSequence(sequenceIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->enabled == enabled) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return false;
    }

    profile->sequences[sequenceIndex].enabled = enabled;

    return true;
}

bool ProfileSequenceManager::SetSequenceLoop(
    uint32_t sequenceIndex,
    bool loop
) {
    const EffectSequence* current =
        FindSequence(sequenceIndex);

    if (current == nullptr) {
        return false;
    }

    if (current->loop == loop) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return false;
    }

    profile->sequences[sequenceIndex].loop = loop;

    return true;
}

int ProfileSequenceManager::AddStep(
    uint32_t sequenceIndex,
    uint32_t placementId,
    uint32_t effectIndex,
    uint32_t durationMs
) {
    if (placementId == 0 || durationMs == 0) {
        return -1;
    }

    const VehicleProfile* readable =
        GetReadableProfile();

    if (readable == nullptr ||
        sequenceIndex >= readable->sequences.size()) {
        return -1;
    }

    bool effectExists = false;

    for (const LedPlacement& placement :
         readable->placements) {
        if (placement.id != placementId) {
            continue;
        }

        if (effectIndex < placement.effects.size()) {
            effectExists = true;
        }

        break;
    }

    if (!effectExists) {
        return -1;
    }

    if (readable->sequences[sequenceIndex].steps.size() >=
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        return -1;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return -1;
    }

    EffectSequenceStep step;
    step.placementId = placementId;
    step.effectIndex = effectIndex;
    step.durationMs = durationMs;

    auto& steps = profile->sequences[sequenceIndex].steps;

    const int stepIndex =
        static_cast<int>(steps.size());

    steps.push_back(step);

    return stepIndex;
}

bool ProfileSequenceManager::RemoveStep(
    uint32_t sequenceIndex,
    uint32_t stepIndex
) {
    const VehicleProfile* readable =
        GetReadableProfile();

    if (readable == nullptr ||
        sequenceIndex >= readable->sequences.size() ||
        stepIndex >=
            readable->sequences[sequenceIndex].steps.size()) {
        return false;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return false;
    }

    auto& steps = profile->sequences[sequenceIndex].steps;

    if (stepIndex >= steps.size()) {
        return false;
    }

    steps.erase(steps.begin() + stepIndex);

    return true;
}

bool ProfileSequenceManager::SetStepDuration(
    uint32_t sequenceIndex,
    uint32_t stepIndex,
    uint32_t durationMs
) {
    if (durationMs == 0) {
        return false;
    }

    const VehicleProfile* readable =
        GetReadableProfile();

    if (readable == nullptr ||
        sequenceIndex >= readable->sequences.size() ||
        stepIndex >=
            readable->sequences[sequenceIndex].steps.size()) {
        return false;
    }

    const EffectSequenceStep& current =
        readable->sequences[sequenceIndex].steps[stepIndex];

    if (current.durationMs == durationMs) {
        return true;
    }

    VehicleProfile* profile = GetEditableProfile();

    if (profile == nullptr ||
        sequenceIndex >= profile->sequences.size()) {
        return false;
    }

    auto& steps = profile->sequences[sequenceIndex].steps;

    if (stepIndex >= steps.size()) {
        return false;
    }

    steps[stepIndex].durationMs = durationMs;

    return true;
}

} // namespace VLS
