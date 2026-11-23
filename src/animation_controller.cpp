#include "animation_controller.h"

#include "pattern_registry.h"

#include <vector>
#include <cstddef>

namespace VLS {
namespace {

struct Candidate {
    uint32_t effectIndex;
    LedEffectType type;
};

void BuildCandidates(
    const LedPlacement& placement,
    std::vector<Candidate>& candidates,
    bool registryOrder
) {
    if (!placement.enabled)
        return;

    if (registryOrder) {
        size_t patternCount = 0;
        const PatternInfo* patterns = PatternRegistry::GetPatterns(patternCount);
        if (patterns == nullptr)
            return;
        for (size_t p = 0; p < patternCount; ++p) {
            for (size_t i = 0; i < placement.effects.size(); ++i) {
                const LedEffectConfig& effect = placement.effects[i];
                if (!effect.enabled || effect.type != patterns[p].type)
                    continue;
                if (!PatternRegistry::IsCompatible(patterns[p], placement.zone))
                    continue;
                candidates.push_back({static_cast<uint32_t>(i), effect.type});
            }
        }
        return;
    }

    for (size_t i = 0; i < placement.effects.size(); ++i) {
        const LedEffectConfig& effect = placement.effects[i];
        if (!effect.enabled || PatternRegistry::FindPattern(effect.type) == nullptr)
            continue;
        if (!PatternRegistry::IsCompatible(effect.type, placement.zone))
            continue;
        candidates.push_back({static_cast<uint32_t>(i), effect.type});
    }
}

uint32_t NextRandom(uint32_t& state) {
    if (state == 0)
        state = 0xA341316Cu;
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

AnimationMode AnimationController::GetMode() const { return m_mode; }

void AnimationController::SetIntervalMs(uint32_t intervalMs) {
    if (intervalMs < 100) intervalMs = 100;
    m_intervalMs = intervalMs;
}

uint32_t AnimationController::GetIntervalMs() const { return m_intervalMs; }

void AnimationController::SetManualSelection(uint32_t placementId, uint32_t effectIndex) {
    m_manualPlacementId = placementId;
    m_manualEffectIndex = effectIndex;
    if (m_mode == AnimationMode::Manual)
        m_hasSelection = false;
}

void AnimationController::SetRandomSeed(uint32_t seed) {
    m_randomState = seed ? seed : 0xA341316Cu;
}

bool AnimationController::Update(const LedPlacement& placement, uint32_t nowMs) {
    if (!placement.enabled) {
        m_hasSelection = false;
        m_selection = AnimationSelection{};
        return false;
    }

    std::vector<Candidate> candidates;
    const bool registryOrder = (m_mode == AnimationMode::Automatic);
    BuildCandidates(placement, candidates, registryOrder);

    if (candidates.empty()) {
        m_hasSelection = false;
        m_selection = AnimationSelection{};
        return false;
    }

    uint32_t activeIntervalMs = m_intervalMs;
    if (m_mode == AnimationMode::Custom && m_hasSelection) {
        for (const LedEffectConfig& effect : placement.effects) {
            if (effect.enabled && effect.type == m_selection.type &&
                effect.durationMs >= 100 && effect.durationMs <= 60000) {
                // The current effect's duration controls how long this LED holds its step.
                if (&effect == &placement.effects[m_selection.effectIndex]) {
                    activeIntervalMs = effect.durationMs;
                    break;
                }
            }
        }
    }

    if (m_mode == AnimationMode::Manual) {
        uint32_t requested = m_manualEffectIndex;
        if (m_manualPlacementId != placement.id)
            requested = 0;
        bool found = false;
        for (const Candidate& candidate : candidates) {
            if (candidate.effectIndex == requested) {
                m_currentCandidate = static_cast<uint32_t>(&candidate - candidates.data());
                found = true;
                break;
            }
        }
        if (!found)
            m_currentCandidate = 0;
    } else if (!m_hasSelection) {
        m_currentCandidate = 0;
    } else if (nowMs - m_lastChangeMs >= activeIntervalMs && candidates.size() > 1) {
        if (m_mode == AnimationMode::Random) {
            uint32_t next = NextRandom(m_randomState) % static_cast<uint32_t>(candidates.size());
            if (next == m_currentCandidate)
                next = (next + 1) % static_cast<uint32_t>(candidates.size());
            m_currentCandidate = next;
        } else {
            // Sequential and Custom use this placement's own effect order.
            // The dedicated editable Custom Sequence editor is the next layer.
            m_currentCandidate = (m_currentCandidate + 1) % static_cast<uint32_t>(candidates.size());
        }
    }

    if (m_currentCandidate >= candidates.size())
        m_currentCandidate = 0;

    const Candidate& selected = candidates[m_currentCandidate];
    m_selection.valid = true;
    m_selection.placementId = placement.id;
    m_selection.effectIndex = selected.effectIndex;
    m_selection.type = selected.type;
    m_hasSelection = true;

    if (m_mode != AnimationMode::Manual)
        m_lastChangeMs = (m_lastChangeMs == 0 || nowMs - m_lastChangeMs >= activeIntervalMs)
            ? nowMs : m_lastChangeMs;

    return true;
}

const AnimationSelection& AnimationController::GetSelection() const { return m_selection; }

void AnimationController::Reset() {
    m_hasSelection = false;
    m_currentCandidate = 0;
    m_lastChangeMs = 0;
    m_selection = AnimationSelection{};
}

} // namespace VLS
