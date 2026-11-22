#pragma once

#include <cstdint>

#include "vehicle_profile.h"

namespace VLS {

struct MatrixPixelOutput {
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;
    float intensity = 0.0f;
    uint8_t alpha = 255;
    bool visible = false;
};

class MatrixAnimationEngine {
public:
    static bool EvaluatePixel(
        const LedEffectConfig& effect,
        uint16_t column,
        uint16_t row,
        uint32_t timeMs,
        MatrixPixelOutput& output
    );
};

} // namespace VLS
