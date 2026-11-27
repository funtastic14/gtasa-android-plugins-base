#pragma once

#include <cmath>
#include <cstdint>

namespace VLS {

enum class EffectTransitionMode : uint8_t {
    Crossfade = 0,
    Wipe = 1,
    Shutter = 2,
    Radial = 3,
    PixelDissolve = 4
};

enum class EffectTransitionDirection : uint8_t {
    LeftToRight = 0,
    RightToLeft = 1,
    TopToBottom = 2,
    BottomToTop = 3,
    CenterOut = 4,
    EdgeToCenter = 5
};

// Shared, allocation-free transition math. Rendering systems provide normalized
// coordinates and use the returned coverage as an alpha multiplier.
class TransitionEngine {
public:
    static float Clamp01(float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    static float SmoothStep(float value) {
        const float t = Clamp01(value);
        return t * t * (3.0f - 2.0f * t);
    }

    static uint32_t Hash(uint32_t value) {
        value ^= value >> 16;
        value *= 0x7FEB352Du;
        value ^= value >> 15;
        value *= 0x846CA68Bu;
        value ^= value >> 16;
        return value;
    }

    // Returns incoming visibility in [0,1]. Outgoing visibility is its inverse.
    // x/y must be normalized to [0,1]; columns/rows define stable dissolve cells.
    static float EvaluateIncoming(
        EffectTransitionMode mode,
        EffectTransitionDirection direction,
        float progress,
        float x,
        float y,
        uint16_t column,
        uint16_t row,
        uint16_t columns,
        uint16_t rows,
        uint32_t seed
    ) {
        const float clampedProgress = Clamp01(progress);
        if (clampedProgress <= 0.0f)
            return 0.0f;
        if (clampedProgress >= 1.0f)
            return 1.0f;
        const float p = SmoothStep(clampedProgress);
        x = Clamp01(x);
        y = Clamp01(y);
        constexpr float edge = 0.035f;

        switch (mode) {
        case EffectTransitionMode::Crossfade:
            return p;

        case EffectTransitionMode::Wipe: {
            float position = x;
            if (direction == EffectTransitionDirection::RightToLeft)
                position = 1.0f - x;
            else if (direction == EffectTransitionDirection::TopToBottom)
                position = y;
            else if (direction == EffectTransitionDirection::BottomToTop)
                position = 1.0f - y;
            else if (direction == EffectTransitionDirection::CenterOut)
                position = std::fabs(x - 0.5f) * 2.0f;
            else if (direction == EffectTransitionDirection::EdgeToCenter)
                position = 1.0f - std::fabs(x - 0.5f) * 2.0f;
            return SmoothStep((p - position + edge) / (2.0f * edge));
        }

        case EffectTransitionMode::Shutter: {
            float axis = (direction == EffectTransitionDirection::TopToBottom ||
                          direction == EffectTransitionDirection::BottomToTop)
                ? y : x;
            if (direction == EffectTransitionDirection::RightToLeft ||
                direction == EffectTransitionDirection::BottomToTop)
                axis = 1.0f - axis;
            const float opening = direction == EffectTransitionDirection::CenterOut
                ? std::fabs(axis - 0.5f) * 2.0f
                : (direction == EffectTransitionDirection::EdgeToCenter
                    ? 1.0f - std::fabs(axis - 0.5f) * 2.0f : axis);
            const float slats = 8.0f;
            const float local = opening * slats;
            const float slatPhase = local - std::floor(local);
            const float threshold = p * 1.35f - 0.175f;
            return SmoothStep((threshold - slatPhase + edge) / (2.0f * edge));
        }

        case EffectTransitionMode::Radial: {
            const float dx = x - 0.5f;
            const float dy = y - 0.5f;
            const float radius = std::sqrt(dx * dx + dy * dy) * 1.41421356f;
            if (direction == EffectTransitionDirection::EdgeToCenter)
                return SmoothStep((radius - (1.0f - p) + edge) / (2.0f * edge));
            return SmoothStep((p - radius + edge) / (2.0f * edge));
        }

        case EffectTransitionMode::PixelDissolve: {
            const uint32_t safeColumns = columns == 0 ? 1u : columns;
            const uint32_t safeRows = rows == 0 ? 1u : rows;
            const uint32_t cell = static_cast<uint32_t>(row) * safeColumns +
                static_cast<uint32_t>(column);
            const uint32_t random = Hash(seed ^ (cell * 0x9E3779B9u));
            const float threshold = static_cast<float>(random & 0x00FFFFFFu) /
                16777215.0f;
            return SmoothStep((p - threshold + 0.04f) / 0.08f);
        }
        }
        return p;
    }

    static float EvaluateOutgoing(
        EffectTransitionMode mode,
        EffectTransitionDirection direction,
        float progress,
        float x,
        float y,
        uint16_t column,
        uint16_t row,
        uint16_t columns,
        uint16_t rows,
        uint32_t seed
    ) {
        return 1.0f - EvaluateIncoming(
            mode, direction, progress, x, y,
            column, row, columns, rows, seed
        );
    }
};

} // namespace VLS
