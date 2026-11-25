#pragma once

#include "vehicle_profile.h"

#include <algorithm>
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
    const uint32_t count = static_cast<uint32_t>(LedMotionPattern::OrbitRing) + 1u;
    if (playback == MotionPlaybackMode::Sequential)
        return static_cast<LedMotionPattern>(step % count);
    if (playback == MotionPlaybackMode::Automatic) {
        static const LedMotionPattern automaticOrder[] = {
            LedMotionPattern::LeftToRight, LedMotionPattern::CenterOut,
            LedMotionPattern::ZoomIn, LedMotionPattern::Wave,
            LedMotionPattern::Ripple, LedMotionPattern::Windmill,
            LedMotionPattern::Spiral, LedMotionPattern::Starburst,
            LedMotionPattern::LaserWrite, LedMotionPattern::LaserWeld,
            LedMotionPattern::ArrowLeft, LedMotionPattern::ArrowRight,
            LedMotionPattern::ArrowUp, LedMotionPattern::ArrowDown,
            LedMotionPattern::OpposingArrows,
            LedMotionPattern::CenterCollision, LedMotionPattern::CenterBurst,
            LedMotionPattern::DiagonalCollision, LedMotionPattern::DiagonalCenterBurst,
            LedMotionPattern::MultiCollision, LedMotionPattern::CrossBurst,
            LedMotionPattern::RadialBurst, LedMotionPattern::PingPong,
            LedMotionPattern::MirrorReflection, LedMotionPattern::ReflectiveWave,
            LedMotionPattern::CollisionRebound, LedMotionPattern::CenterSplit,
            LedMotionPattern::DualFlow, LedMotionPattern::MirroredDiagonal,
            LedMotionPattern::DoubleRebound,
            LedMotionPattern::VerticalSweep, LedMotionPattern::SideCollision,
            LedMotionPattern::CornerBurst, LedMotionPattern::TwinSpiral,
            LedMotionPattern::OrbitRing,
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

inline float GetMotionPhase(uint32_t timeMs, float speed) {
    const double cycles = static_cast<double>(timeMs) *
        static_cast<double>(speed) / 1800.0;
    return static_cast<float>(cycles - std::floor(cycles));
}

inline MotionFrameState BuildMotionFrameState(
    const LedEffectConfig& effect,
    uint32_t timeMs,
    MotionPlaybackMode playback,
    uint32_t intervalMs
) {
    MotionFrameState state;
    state.timeMs = timeMs;
    state.intervalMs = intervalMs < 500u ? 500u : intervalMs;
    state.cycleIndex = timeMs / state.intervalMs;
    state.phase = GetMotionPhase(timeMs, effect.speed);
    state.pattern = ResolveMotionPattern(effect, timeMs, playback, state.intervalMs);
    return state;
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
        // One broad wave across the placement instead of two tightly packed cycles.
        return 0.5f + 0.5f * std::sin(wavePhase + x * PI2);
    case LedMotionPattern::Diagonal: {
        const float d = fract(phase - fract(x + y));
        return d < 0.25f ? 1.0f - d / 0.25f : 0.03f;
    }
    case LedMotionPattern::CenterCollision:
    case LedMotionPattern::DiagonalCollision: {
        // Two opposing fronts converge on the center, flash at impact,
        // then send one expanding shockwave back along the same axis.
        const float axis = pattern == LedMotionPattern::DiagonalCollision
            ? (x + y) * 0.5f : x;
        const float distanceFromCenter = std::fabs(axis - 0.5f);
        if (phase < 0.68f) {
            const float front = 0.5f * (1.0f - phase / 0.68f);
            const float beam = clamp01(1.0f -
                std::fabs(distanceFromCenter - front) * 20.0f);
            return 0.025f + 0.975f * beam;
        }
        if (phase < 0.78f) {
            const float flashPhase = (phase - 0.68f) / 0.10f;
            return 0.35f + 0.65f * (0.5f + 0.5f *
                std::sin(flashPhase * PI2));
        }
        const float shockPhase = (phase - 0.78f) / 0.22f;
        const float shockFront = shockPhase * 0.5f;
        const float shock = clamp01(1.0f -
            std::fabs(distanceFromCenter - shockFront) * 17.0f);
        return 0.025f + 0.975f * shock;
    }
    case LedMotionPattern::CenterBurst:
    case LedMotionPattern::DiagonalCenterBurst: {
        // A center-originating wave expands outward. The diagonal variant
        // adds a moving diagonal ignition sweep before the radial burst.
        const float radialDistance = std::sqrt(dx * dx + dy * dy) * 1.41421356f;
        if (pattern == LedMotionPattern::DiagonalCenterBurst && phase < 0.28f) {
            const float sweep = fract(phase / 0.28f - fract(x + y));
            return sweep < 0.18f ? 1.0f - sweep / 0.18f : 0.025f;
        }
        const float burstPhase = pattern == LedMotionPattern::DiagonalCenterBurst
            ? (phase - 0.28f) / 0.72f : phase;
        const float front = burstPhase;
        const float wave = clamp01(1.0f -
            std::fabs(radialDistance - front) * 13.0f);
        return 0.025f + 0.975f * wave;
    }
    case LedMotionPattern::MultiCollision: {
        const float edgeDistance = std::min(std::min(x, 1.0f - x), std::min(y, 1.0f - y));
        if (phase < 0.62f) {
            const float front = (phase / 0.62f) * 0.5f;
            const float beam = clamp01(1.0f - std::fabs(edgeDistance - front) * 18.0f);
            return 0.025f + 0.975f * beam;
        }
        if (phase < 0.72f) return 1.0f;
        const float shock = (phase - 0.72f) / 0.28f;
        return 0.025f + 0.975f * clamp01(1.0f - std::fabs(radius - shock) * 14.0f);
    }
    case LedMotionPattern::CrossBurst: {
        const float front = phase * 0.5f;
        const float horizontal = clamp01(1.0f - std::fabs(std::fabs(y - 0.5f) - front) * 20.0f);
        const float vertical = clamp01(1.0f - std::fabs(std::fabs(x - 0.5f) - front) * 20.0f);
        return 0.025f + 0.975f * std::max(horizontal, vertical);
    }
    case LedMotionPattern::RadialBurst: {
        const float first = clamp01(1.0f - std::fabs(radius - phase) * 18.0f);
        const float second = clamp01(1.0f - std::fabs(radius - (phase * 0.72f + 0.22f)) * 18.0f);
        return 0.025f + 0.975f * std::max(first, second * 0.72f);
    }
    case LedMotionPattern::PingPong: {
        const float travel = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
        const float d = std::fabs(x - travel);
        return d < 0.13f ? 1.0f - d / 0.13f : 0.035f;
    }
    case LedMotionPattern::MirrorReflection: {
        const float unfolded = fract(phase * 2.0f + x);
        const float reflected = unfolded < 0.5f ? unfolded * 2.0f : (1.0f - unfolded) * 2.0f;
        const float d = std::fabs(reflected - 0.5f);
        return clamp01(1.0f - d * 3.8f);
    }
    case LedMotionPattern::ReflectiveWave: {
        const float standing = std::sin(x * PI2 * 3.0f) * std::cos(wavePhase * 1.5f);
        const float moving = std::sin(x * PI2 * 2.0f - wavePhase);
        return 0.08f + 0.92f * (0.5f + 0.25f * standing + 0.25f * moving);
    }
    case LedMotionPattern::CollisionRebound: {
        if (phase < 0.34f) {
            const float front = 0.5f * (1.0f - phase / 0.34f);
            return 0.025f + 0.975f * clamp01(1.0f - std::fabs(std::fabs(x - 0.5f) - front) * 22.0f);
        }
        if (phase < 0.43f) return 1.0f;
        const float reboundPhase = (phase - 0.43f) / 0.57f;
        const float front = reboundPhase < 0.62f
            ? reboundPhase / 0.62f * 0.5f
            : (1.0f - (reboundPhase - 0.62f) / 0.38f) * 0.5f;
        return 0.025f + 0.975f * clamp01(1.0f - std::fabs(std::fabs(x - 0.5f) - front) * 18.0f);
    }
    case LedMotionPattern::CenterSplit: {
        const float front = phase * 0.5f;
        const float d = std::fabs(std::fabs(x - 0.5f) - front);
        return d < 0.12f ? 1.0f - d / 0.12f : 0.03f;
    }
    case LedMotionPattern::DualFlow: {
        const float left = fract(x * 2.0f - phase * 2.0f);
        const float right = fract((1.0f - x) * 2.0f - phase * 2.0f);
        const float a = left < 0.24f ? 1.0f - left / 0.24f : 0.04f;
        const float b = right < 0.24f ? 1.0f - right / 0.24f : 0.04f;
        return std::max(a, b);
    }
    case LedMotionPattern::MirroredDiagonal: {
        const float axis = x < 0.5f ? (x + y) * 0.5f : ((1.0f - x) + y) * 0.5f;
        const float d = fract(phase - axis);
        return d < 0.22f ? 1.0f - d / 0.22f : 0.035f;
    }
    case LedMotionPattern::DoubleRebound: {
        const float triangle = phase < 0.25f ? phase * 4.0f :
            (phase < 0.5f ? 1.0f - (phase - 0.25f) * 4.0f :
             (phase < 0.75f ? (phase - 0.5f) * 4.0f : 1.0f - (phase - 0.75f) * 4.0f));
        const float d = std::fabs(x - triangle);
        return d < 0.11f ? 1.0f - d / 0.11f : 0.03f;
    }
    case LedMotionPattern::VerticalSweep: {
        const float d = fract(phase - (1.0f - y));
        return d < 0.20f ? 1.0f - d / 0.20f : 0.025f;
    }
    case LedMotionPattern::SideCollision: {
        const float distanceFromCenter = std::fabs(y - 0.5f);
        if (phase < 0.62f) {
            const float front = 0.5f * (1.0f - phase / 0.62f);
            return 0.025f + 0.975f * clamp01(
                1.0f - std::fabs(distanceFromCenter - front) * 22.0f
            );
        }
        if (phase < 0.72f) return 1.0f;
        const float rebound = (phase - 0.72f) / 0.28f;
        return 0.025f + 0.975f * clamp01(
            1.0f - std::fabs(distanceFromCenter - rebound * 0.5f) * 18.0f
        );
    }
    case LedMotionPattern::CornerBurst: {
        const float cx = std::min(x, 1.0f - x);
        const float cy = std::min(y, 1.0f - y);
        const float cornerRadius = std::sqrt(cx * cx + cy * cy) * 1.41421356f;
        const float wave = clamp01(1.0f - std::fabs(cornerRadius - phase) * 16.0f);
        return 0.025f + 0.975f * wave;
    }
    case LedMotionPattern::TwinSpiral: {
        const float angle = std::atan2(dy, dx);
        const float armA = std::cos(radius * 24.0f - angle * 2.0f - wavePhase);
        const float armB = std::cos(radius * 24.0f + angle * 2.0f - wavePhase);
        return 0.04f + 0.96f * (0.5f + 0.5f * std::max(armA, armB));
    }
    case LedMotionPattern::OrbitRing: {
        const float angle = std::atan2(dy, dx);
        float angularDistance = std::fabs(angle - wavePhase);
        if (angularDistance > 3.14159265f)
            angularDistance = 6.28318530f - angularDistance;
        const float ring = clamp01(1.0f - std::fabs(radius - 0.62f) * 12.0f);
        const float orbitHead = clamp01(1.0f - angularDistance * 2.5f);
        return 0.035f + 0.965f * ring * (0.35f + 0.65f * orbitHead);
    }
    case LedMotionPattern::Ripple:
        return 0.5f + 0.5f * std::sin(radius * 18.0f - wavePhase * 2.0f);
    case LedMotionPattern::Pulse:
        return 0.08f + 0.92f * (0.5f + 0.5f * std::sin(wavePhase));
    case LedMotionPattern::AlternateRows:
        return ((row + static_cast<uint16_t>(phase * 4.0f)) % 2u) ? 0.08f : 1.0f;
    case LedMotionPattern::AlternateColumns:
        return ((column + static_cast<uint16_t>(phase * 4.0f)) % 2u) ? 0.08f : 1.0f;
    case LedMotionPattern::Checkerboard: {
        // Use larger 3x3 pixel tiles; the matrix renderer adds a wider
        // physical gap between each illuminated tile.
        const uint16_t blockColumn = column / 3u;
        const uint16_t blockRow = row / 3u;
        const uint16_t phaseStep = static_cast<uint16_t>(phase * 4.0f);
        return ((blockColumn + blockRow + phaseStep) % 2u) ? 0.08f : 1.0f;
    }
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
    case LedMotionPattern::LaserWrite: {
        // A bright writing head leaves a short, fading trail behind it.
        const float distance = phase - x;
        if (std::fabs(distance) < 0.025f)
            return 1.0f;
        if (distance >= 0.0f && distance < 0.30f)
            return 0.12f + 0.88f * (1.0f - distance / 0.30f);
        return 0.025f;
    }
    case LedMotionPattern::LaserWeld: {
        // A diagonal welding path with a hot core, molten trail and tiny sparks.
        const float path = (x + y) * 0.5f;
        const float distance = phase - path;
        if (std::fabs(distance) < 0.035f)
            return 1.0f;
        if (distance >= 0.0f && distance < 0.19f) {
            const float trail = 1.0f - distance / 0.19f;
            const float spark = fract(x * 37.0f + y * 19.0f + phase * 11.0f);
            return spark > 0.88f ? 1.0f : 0.18f + 0.72f * trail;
        }
        return 0.025f;
    }
    case LedMotionPattern::ArrowRight:
    case LedMotionPattern::ArrowLeft:
    case LedMotionPattern::ArrowUp:
    case LedMotionPattern::ArrowDown:
    case LedMotionPattern::OpposingArrows: {
        // Full-matrix arrow field: repeated arrow glyphs occupy the whole
        // matrix. The glyph field itself translates, rather than merely
        // animating color inside one stationary arrow.
        const auto lineDistance = [&](float px, float py, float ax, float ay, float bx, float by) {
            const float vx = bx - ax, vy = by - ay;
            const float wx = px - ax, wy = py - ay;
            const float length2 = vx * vx + vy * vy;
            const float t = length2 > 0.0001f
                ? clamp01((wx * vx + wy * vy) / length2) : 0.0f;
            const float dxLine = px - (ax + t * vx);
            const float dyLine = py - (ay + t * vy);
            return std::sqrt(dxLine * dxLine + dyLine * dyLine);
        };
        const auto arrowShape = [&](float u, float v) {
            const float upper = lineDistance(u, v, 0.16f, 0.12f, 0.84f, 0.50f);
            const float lower = lineDistance(u, v, 0.84f, 0.50f, 0.16f, 0.88f);
            const float first = std::min(upper, lower);
            const float innerUpper = lineDistance(u, v, 0.38f, 0.12f, 0.94f, 0.50f);
            const float innerLower = lineDistance(u, v, 0.94f, 0.50f, 0.38f, 0.88f);
            const float second = std::min(innerUpper, innerLower);
            return first < 0.095f || (columns >= 20u && second < 0.075f);
        };

        // Select a compact grid based on the actual matrix resolution.
        // Clamp counts to keep each arrow large enough to read.
        const float tileColumns = static_cast<float>(
            columns < 8u ? 2u : (columns < 16u ? 3u : 4u));
        const float tileRows = static_cast<float>(
            rows < 6u ? 1u : (rows < 12u ? 2u : 3u));
        const float zoom = 0.86f + 0.14f * (0.5f + 0.5f *
            std::sin(wavePhase));
        const float rowPhase = std::floor(y * tileRows);
        float u = 0.0f;
        float v = 0.0f;
        float axis = 0.0f;
        bool pointsRight = true;

        if (pattern == LedMotionPattern::OpposingArrows) {
            // Two full-height fields travel toward the center:
            // left half points/moves right, right half points/moves left.
            const bool rightHalf = x >= 0.5f;
            const float halfX = rightHalf ? (x - 0.5f) * 2.0f : x * 2.0f;
            const float halfColumns = tileColumns;
            const float directionPhase = rightHalf ? -phase : phase;
            u = fract(halfX * halfColumns - directionPhase * halfColumns
                + rowPhase * 0.5f);
            v = fract(y * tileRows);
            pointsRight = !rightHalf;
            axis = u;
            if (!pointsRight)
                axis = 1.0f - u;
        } else if (pattern == LedMotionPattern::ArrowRight ||
                   pattern == LedMotionPattern::ArrowLeft) {
            const float directionPhase =
                pattern == LedMotionPattern::ArrowRight ? phase : -phase;
            u = fract(x * tileColumns - directionPhase * tileColumns
                + rowPhase * 0.5f);
            v = fract(y * tileRows);
            pointsRight = pattern == LedMotionPattern::ArrowRight;
            axis = pointsRight ? u : 1.0f - u;
        } else {
            const float directionPhase =
                pattern == LedMotionPattern::ArrowDown ? phase : -phase;
            // Alternate columns slightly so the vertical stream has a
            // staggered, conveyor-like rhythm instead of a rigid block.
            const float columnPhase = std::floor(x * tileColumns) * 0.5f;
            v = fract(y * tileRows - directionPhase * tileRows
                + columnPhase);
            u = fract(x * tileColumns);
            pointsRight = pattern == LedMotionPattern::ArrowDown;
            axis = pointsRight ? v : 1.0f - v;
        }

        // A subtle breathing scale makes the whole arrow field feel alive.
        // Coordinates stay inside each tile, avoiding per-frame allocations.
        u = (u - 0.5f) / zoom + 0.5f;
        v = (v - 0.5f) / zoom + 0.5f;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            return 0.018f;

        // Rotate the same glyph for vertical directions. For left-facing
        // arrows mirror the longitudinal coordinate.
        float shapeU = u;
        float shapeV = v;
        if (pattern == LedMotionPattern::ArrowLeft ||
            (pattern == LedMotionPattern::OpposingArrows && !pointsRight))
            shapeU = 1.0f - shapeU;
        if (pattern == LedMotionPattern::ArrowUp ||
            pattern == LedMotionPattern::ArrowDown) {
            shapeU = pointsRight ? v : 1.0f - v;
            shapeV = u;
        }

        if (!arrowShape(shapeU, shapeV))
            return 0.018f;

        // Moving highlights travel along each arrow while brightness pulses
        // are staggered by row/column to add depth without hiding the glyph.
        const float cellIndex = pattern == LedMotionPattern::ArrowUp ||
            pattern == LedMotionPattern::ArrowDown
                ? std::floor(x * tileColumns)
                : rowPhase;
        const float highlight = fract(axis * 1.35f - phase * 2.4f
            + cellIndex * 0.23f);
        const float pulse = 0.72f + 0.28f *
            (0.5f + 0.5f * std::sin(wavePhase + cellIndex * 1.7f));
        return (highlight < 0.34f ? 1.0f : 0.68f) * pulse;
    }
    }
    (void)columns;
    (void)rows;
    return 1.0f;
}

} // namespace VLS
