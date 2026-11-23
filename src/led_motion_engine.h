#pragma once

#include "vehicle_profile.h"

#include <cmath>
#include <cstdint>

namespace VLS {

inline float GetColorPosition(
    LedColorDirection direction,
    float x,
    float y,
    float spread
) {
    const auto clamp01 = [](float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    };
    float position = x;
    switch (direction) {
    case LedColorDirection::LeftToRight: position = x; break;
    case LedColorDirection::RightToLeft: position = 1.0f - x; break;
    case LedColorDirection::TopToBottom: position = y; break;
    case LedColorDirection::BottomToTop: position = 1.0f - y; break;
    case LedColorDirection::DiagonalDown: position = (x + y) * 0.5f; break;
    case LedColorDirection::DiagonalUp: position = (x + (1.0f - y)) * 0.5f; break;
    case LedColorDirection::CenterOut: position = std::fabs(x - 0.5f) * 2.0f; break;
    case LedColorDirection::EdgeToCenter: position = 1.0f - std::fabs(x - 0.5f) * 2.0f; break;
    }
    (void)spread;
    return clamp01(position);
}

// Single source of truth for spatial motion. Both the strip renderer and
// matrix renderer sample this field using normalized placement coordinates.
inline LedMotionPattern ResolveMotionPattern(
    const LedEffectConfig& effect,
    uint32_t timeMs,
    MotionPlaybackMode playback,
    uint32_t intervalMs
) {
    if (playback == MotionPlaybackMode::Manual)
        return effect.motionPattern;
    const uint32_t interval = intervalMs < 500 ? 500 : intervalMs;
    const uint32_t step = timeMs / interval;
    const uint32_t count = static_cast<uint32_t>(LedMotionPattern::Starburst) + 1u;
    if (playback == MotionPlaybackMode::Sequential)
        return static_cast<LedMotionPattern>(step % count);
    if (playback == MotionPlaybackMode::Automatic) {
        static const LedMotionPattern automaticOrder[] = {
            LedMotionPattern::LeftToRight, LedMotionPattern::CenterOut,
            LedMotionPattern::ZoomIn, LedMotionPattern::Wave,
            LedMotionPattern::Ripple, LedMotionPattern::Windmill,
            LedMotionPattern::Spiral, LedMotionPattern::Starburst,
            LedMotionPattern::RightToLeft, LedMotionPattern::EdgeToCenter,
            LedMotionPattern::ZoomOut, LedMotionPattern::Flow
        };
        const uint32_t size = static_cast<uint32_t>(sizeof(automaticOrder) / sizeof(automaticOrder[0]));
        return automaticOrder[step % size];
    }
    uint32_t value = step ^ (effect.motionPattern == LedMotionPattern::LeftToRight ? 0x9E3779B9u : 0x85EBCA6Bu);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return static_cast<LedMotionPattern>(value % count);
}

inline float EvaluateMotionIntensity(
    LedMotionPattern pattern,
    float x,
    float y,
    float phase,
    uint16_t column,
    uint16_t row,
    uint16_t columns,
    uint16_t rows
) {
    constexpr float PI2 = 6.28318530717958647692f;
    const float wavePhase = PI2 * phase;
    const float dx = x - 0.5f;
    const float dy = y - 0.5f;
    const float radius = std::sqrt(dx * dx + dy * dy) * 1.41421356f;
    const auto fract = [](float value) {
        return value - std::floor(value);
    };
    const auto clamp01 = [](float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    };

    switch (pattern) {
    case LedMotionPattern::LeftToRight: {
        const float d = fract(phase - x);
        return d < 0.24f ? 1.0f - d / 0.24f : 0.035f;
    }
    case LedMotionPattern::RightToLeft: {
        const float d = fract(phase - (1.0f - x));
        return d < 0.24f ? 1.0f - d / 0.24f : 0.035f;
    }
    case LedMotionPattern::CenterOut: {
        const float d = fract(phase - std::fabs(x - 0.5f) * 2.0f);
        return d < 0.22f ? 1.0f - d / 0.22f : 0.025f;
    }
    case LedMotionPattern::EdgeToCenter: {
        const float d = fract(phase - (1.0f - std::fabs(x - 0.5f) * 2.0f));
        return d < 0.22f ? 1.0f - d / 0.22f : 0.025f;
    }
    case LedMotionPattern::ZoomIn:
        return clamp01(1.0f - std::fabs(radius - phase) * 5.0f);
    case LedMotionPattern::ZoomOut:
        return clamp01(1.0f - std::fabs(radius - (1.0f - phase)) * 5.0f);
    case LedMotionPattern::FadeIn:
        return phase;
    case LedMotionPattern::FadeOut:
        return 1.0f - phase;
    case LedMotionPattern::Rolling:
        return fract(x * 3.0f - phase) < 0.48f ? 1.0f : 0.04f;
    case LedMotionPattern::Flow:
        return 0.15f + 0.85f * (0.5f + 0.5f *
            std::sin(wavePhase - (x + y) * PI2 * 2.0f));
    case LedMotionPattern::Wave:
        return 0.5f + 0.5f * std::sin(wavePhase + x * PI2 * 2.0f);
    case LedMotionPattern::Diagonal: {
        const float d = fract(phase - fract(x + y));
        return d < 0.25f ? 1.0f - d / 0.25f : 0.03f;
    }
    case LedMotionPattern::Ripple:
        return 0.5f + 0.5f * std::sin(radius * 18.0f - wavePhase * 2.0f);
    case LedMotionPattern::Pulse:
        return 0.08f + 0.92f * (0.5f + 0.5f * std::sin(wavePhase));
    case LedMotionPattern::AlternateRows:
        return ((row + static_cast<uint16_t>(phase * 4.0f)) % 2u) ? 0.08f : 1.0f;
    case LedMotionPattern::AlternateColumns:
        return ((column + static_cast<uint16_t>(phase * 4.0f)) % 2u) ? 0.08f : 1.0f;
    case LedMotionPattern::Checkerboard:
        return ((column + row + static_cast<uint16_t>(phase * 4.0f)) % 2u) ? 0.08f : 1.0f;
    case LedMotionPattern::Windmill: {
        const float angle = std::atan2(dy, dx);
        const float blade = std::cos(angle * 4.0f - wavePhase);
        return blade > 0.15f ? 1.0f : 0.08f;
    }
    case LedMotionPattern::Spiral: {
        const float angle = std::atan2(dy, dx);
        const float spiral = std::cos(radius * 24.0f - angle * 3.0f - wavePhase);
        return 0.08f + 0.92f * (0.5f + 0.5f * spiral);
    }
    case LedMotionPattern::Starburst: {
        const float angle = std::atan2(dy, dx);
        const float rays = std::fabs(std::cos(angle * 6.0f - wavePhase * 0.5f));
        const float radial = clamp01(1.0f - radius * 0.55f);
        return 0.06f + 0.94f * rays * radial;
    }
    }
    (void)columns;
    (void)rows;
    return 1.0f;
}

} // namespace VLS
