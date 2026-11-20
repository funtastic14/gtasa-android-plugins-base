
#include "animation_controller.h"

#include "pattern_registry.h"

#include <vector>
#include <cstddef>

namespace VLS {

namespace {

struct Candidate {
    uint32_t placementId;
    uint32_t effectIndex;
    LedEffectType type;
};

bool IsCandidateValid(
    const VehicleProfile& profile,
    const Candidate& candidate
) {
    for (const LedPlacement& placement : profile.placements) {
        if (placement.id != candidate.placementId)
            continue;

        if (candidate.effectIndex >= placement.effects.size())
            return false;

        const LedEffectConfig& effect =
            placement.effects[candidate.effectIndex];

        return effect.enabled &&
               effect.type == candidate.type &&
               PatternRegistry::IsCompatible(
                   effect.type,
                   placement.zone
               );
    }

    return false;
}

void BuildSequentialCandidates(
    const VehicleProfile& profile,
    std::vector<Candidate>& candidates
) {
    for (const LedPlacement& placement : profile.placements) {
        for (size_t i = 0; i < placement.effects.size(); ++i) {
            const LedEffectConfig& effect = placement.effects[i];

            if (!effect.enabled)
                continue;

            if (PatternRegistry::FindPattern(effect.type) == nullptr)
                continue;

            if (!PatternRegistry::IsCompatible(
                    effect.type,
                    placement.zone
                ))
                continue;

            Candidate candidate;

            candidate.placementId = placement.id;
            candidate.effectIndex = static_cast<uint32_t>(i);
            candidate.type = effect.type;

            candidates.push_back(candidate);
        }
    }
}

void BuildAutomaticCandidates(
    const VehicleProfile& profile,
    std::vector<Candidate>& candidates
) {
    size_t patternCount = 0;

    const PatternInfo* patterns =
        PatternRegistry::GetPatterns(patternCount);

    if (patterns == nullptr)
        return;

    // Automatic follows the order of the Pattern Registry.
    for (size_t p = 0; p < patternCount; ++p) {
        for (const LedPlacement& placement : profile.placements) {
            for (size_t i = 0; i < placement.effects.size(); ++i) {
                const LedEffectConfig& effect =
                    placement.effects[i];

                if (!effect.enabled || effect.type != patterns[p].type)
                    continue;

                if (!PatternRegistry::IsCompatible(
                        patterns[p],
                        placement.zone
                    ))
                    continue;

                Candidate candidate;

                candidate.placementId = placement.id;
                candidate.effectIndex = static_cast<uint32_t>(i);
                candidate.type = effect.type;

                candidates.push_back(candidate);
            }
        }
    }
}

uint32_t NextRandom(uint32_t& state) {
    if (state == 0)
        state = 0xA341316Cu;

    // Xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return state;
}

} // namespace

void AnimationController::SetMode(AnimationMode mode) {
    if (m_mode == mode)
        return;

    m_mode = mode;
    m_hasSelection = false;
    m_selection = AnimationSelection{};
}

AnimationMode AnimationController::GetMode() const {
    return m_mode;
}

void AnimationController::SetIntervalMs(uint32_t intervalMs) {
    if (intervalMs < 100)
        intervalMs = 100;

    m_intervalMs = intervalMs;
}

uint32_t AnimationController::GetIntervalMs() const {
    return m_intervalMs;
}

void AnimationController::SetManualSelection(
    uint32_t placementId,
    uint32_t effectIndex
) {
    m_manualPlacementId = placementId;
    m_manualEffectIndex = effectIndex;

    if (m_mode == AnimationMode::Manual)
        m_hasSelection = false;
}

void AnimationController::SetRandomSeed(uint32_t seed) {
    m_randomState = seed;

    if (m_randomState == 0)
        m_randomState = 0xA341316Cu;
}

bool AnimationController::Update(
    const VehicleProfile& profile,
    uint32_t nowMs
) {
    if (!profile.enabled) {
        m_hasSelection = false;
        m_selection = AnimationSelection{};
        return false;
    }

    if (m_mode == AnimationMode::Custom) {
        // Custom selection is provided by ProfileSequenceRuntime.
        m_hasSelection = false;
        m_selection = AnimationSelection{};
        return false;
    }

    std::vector<Candidate> candidates;

    if (m_mode == AnimationMode::Automatic) {
        BuildAutomaticCandidates(profile, candidates);
    } else {
        BuildSequentialCandidates(profile, candidates);
    }

    if (candidates.empty()) {
        m_hasSelection = false;
        m_selection = AnimationSelection{};
        return false;
    }

    // Manual mode keeps the requested placement/effect.
    if (m_mode == AnimationMode::Manual) {
        for (const Candidate& candidate : candidates) {
            if (candidate.placementId == m_manualPlacementId &&
                candidate.effectIndex == m_manualEffectIndex) {

                m_selection.valid = true;
                m_selection.placementId = candidate.placementId;
                m_selection.effectIndex = candidate.effectIndex;
                m_selection.type = candidate.type;

                m_hasSelection = true;
                return true;
            }
        }

        // If the requested selection is no longer valid,
        // use the first available candidate.
        m_currentCandidate = 0;
    } else if (!m_hasSelection) {
        m_currentCandidate = 0;
    } else {
        const uint32_t elapsed =
            nowMs - m_lastChangeMs;

        if (elapsed >= m_intervalMs) {
            if (m_mode == AnimationMode::Random) {
                if (candidates.size() > 1) {
                    uint32_t next =
                        NextRandom(m_randomState) %
                        static_cast<uint32_t>(candidates.size());

                    if (next == m_currentCandidate)
                        next = (next + 1) %
                            static_cast<uint32_t>(candidates.size());

                    m_currentCandidate = next;
                }
            } else {
                m_currentCandidate =
                    (m_currentCandidate + 1) %
                    static_cast<uint32_t>(candidates.size());
            }
        } else if (m_hasSelection) {
            // Preserve the current candidate until its interval expires.
            if (m_currentCandidate < candidates.size()) {
                const Candidate& current =
                    candidates[m_currentCandidate];

                if (IsCandidateValid(profile, current)) {
                    m_selection.valid = true;
                    m_selection.placementId = current.placementId;
                    m_selection.effectIndex = current.effectIndex;
                    m_selection.type = current.type;
                    return true;
                }
            }
        }
    }

    if (m_currentCandidate >= candidates.size())
        m_currentCandidate = 0;

    const Candidate& selected =
        candidates[m_currentCandidate];

    m_selection.valid = true;
    m_selection.placementId = selected.placementId;
    m_selection.effectIndex = selected.effectIndex;
    m_selection.type = selected.type;

    m_hasSelection = true;
    m_lastChangeMs = nowMs;

    return true;
}

const AnimationSelection&
AnimationController::GetSelection() const {
    return m_selection;
}

void AnimationController::Reset() {
    m_hasSelection = false;
    m_currentCandidate = 0;
    m_lastChangeMs = 0;
    m_selection = AnimationSelection{};
}

} // namespace VLS
