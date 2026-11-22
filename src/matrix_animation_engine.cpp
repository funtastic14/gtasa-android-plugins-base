#include "matrix_animation_engine.h"

#include <cmath>
#include <cctype>
#include <cstddef>

namespace VLS {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

float Fract(float value) {
    return value - std::floor(value);
}

uint8_t ClampColor(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 255.0f) value = 255.0f;
    return static_cast<uint8_t>(value);
}

void HSVtoRGB(
    float hue,
    uint8_t& red,
    uint8_t& green,
    uint8_t& blue
) {
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f)
        hue += 360.0f;

    const float c = 1.0f;
    const float x = c * (
        1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f)
    );

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (hue < 60.0f) {
        r = c; g = x;
    } else if (hue < 120.0f) {
        r = x; g = c;
    } else if (hue < 180.0f) {
        g = c; b = x;
    } else if (hue < 240.0f) {
        g = x; b = c;
    } else if (hue < 300.0f) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }

    red = ClampColor(r * 255.0f);
    green = ClampColor(g * 255.0f);
    blue = ClampColor(b * 255.0f);
}

uint8_t GetGlyphRow(char input, uint16_t row) {
    if (row >= 7)
        return 0;
    char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
    static const uint8_t font[36][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    if (ch >= '0' && ch <= '9')
        return font[ch - '0'][row];
    if (ch >= 'A' && ch <= 'Z')
        return font[10 + ch - 'A'][row];
    if (ch == '-') return row == 3 ? 14 : 0;
    if (ch == '.') return row == 6 ? 4 : 0;
    if (ch == ':') return (row == 2 || row == 5) ? 4 : 0;
    if (ch == ' ') return 0;
    return row == 0 || row == 6 ? 31 : 17;
}

float GetDirectionalPosition(
    LedDirection direction,
    float x,
    float y,
    float customAngle
) {
    switch (direction) {
    case LedDirection::LeftToRight:
        return x;

    case LedDirection::RightToLeft:
        return 1.0f - x;

    case LedDirection::TopToBottom:
        return y;

    case LedDirection::BottomToTop:
        return 1.0f - y;

    case LedDirection::CustomAngle:
    default:
        break;
    }

    const float radians = customAngle * PI / 180.0f;
    const float dx = std::cos(radians);
    const float dy = std::sin(radians);

    const float p00 = 0.0f;
    const float p10 = dx;
    const float p01 = dy;
    const float p11 = dx + dy;

    float minimum = p00;
    float maximum = p00;

    if (p10 < minimum) minimum = p10;
    if (p01 < minimum) minimum = p01;
    if (p11 < minimum) minimum = p11;

    if (p10 > maximum) maximum = p10;
    if (p01 > maximum) maximum = p01;
    if (p11 > maximum) maximum = p11;

    const float range = maximum - minimum;
    if (range <= 0.0001f)
        return x;

    const float projected = x * dx + y * dy;
    return Clamp01((projected - minimum) / range);
}

} // namespace

bool MatrixAnimationEngine::EvaluatePixel(
    const LedEffectConfig& effect,
    uint16_t column,
    uint16_t row,
    uint32_t timeMs,
    MatrixPixelOutput& output
) {
    output = MatrixPixelOutput{};

    const LedMatrixConfig& matrix = effect.matrix;

    if (!matrix.enabled ||
        matrix.columns == 0 ||
        matrix.rows == 0 ||
        column >= matrix.columns ||
        row >= matrix.rows)
        return false;

    const uint32_t pixelCount =
        static_cast<uint32_t>(matrix.columns) * matrix.rows;

    if (pixelCount > 4096)
        return false;

    const uint32_t pixelIndex =
        static_cast<uint32_t>(row) * matrix.columns + column;

    LedMatrixPixel defaultPixel;
    defaultPixel.red = effect.red;
    defaultPixel.green = effect.green;
    defaultPixel.blue = effect.blue;
    const LedMatrixPixel* pixel = &defaultPixel;

    if (!matrix.pixels.empty()) {
        if (matrix.pixels.size() != pixelCount ||
            pixelIndex >= matrix.pixels.size())
            return false;

        pixel = &matrix.pixels[pixelIndex];
    }

    if (!pixel->enabled)
        return true;

    if (!std::isfinite(effect.speed) ||
        !std::isfinite(effect.brightness) ||
        !std::isfinite(pixel->speed) ||
        !std::isfinite(pixel->brightness) ||
        !std::isfinite(pixel->customAngle))
        return false;

    const float x =
        (static_cast<float>(column) + 0.5f) /
        static_cast<float>(matrix.columns);

    const float y =
        (static_cast<float>(row) + 0.5f) /
        static_cast<float>(matrix.rows);

    const float directionPosition = GetDirectionalPosition(
        pixel->direction,
        x,
        y,
        pixel->customAngle
    );

    const float speed = effect.speed * pixel->speed;

    const float movement =
        static_cast<float>(timeMs) * speed / 1200.0f;

    const float progress = Fract(movement);
    const float distance = Fract(progress - directionPosition);

    float intensity = 1.0f;

    uint8_t red = pixel->red;
    uint8_t green = pixel->green;
    uint8_t blue = pixel->blue;

    switch (effect.type) {
    case LedEffectType::RunningLED: {
        intensity = 0.06f;

        if (distance < 0.30f) {
            intensity = 1.0f - (distance / 0.30f) * 0.88f;
        }

        break;
    }

    case LedEffectType::StroboWave: {
        const bool flashOn =
            std::fmod(static_cast<float>(timeMs) * speed / 250.0f, 1.0f)
            < 0.5f;

        intensity =
            (distance < 0.22f && flashOn) ? 1.0f : 0.0f;

        break;
    }

    case LedEffectType::StroboEyes: {
        if (matrix.rows >= 7 && matrix.columns >= 14) {
            const float eyeCenterX = column < matrix.columns / 2
                ? static_cast<float>(matrix.columns) * 0.25f
                : static_cast<float>(matrix.columns) * 0.75f;
            const float eyeCenterY = static_cast<float>(matrix.rows - 1) * 0.5f;
            const float eyeRadiusX = static_cast<float>(matrix.columns) * 0.19f;
            const float eyeRadiusY = static_cast<float>(matrix.rows) * 0.43f;
            const float dx = (static_cast<float>(column) - eyeCenterX) / eyeRadiusX;
            const float dy = (static_cast<float>(row) - eyeCenterY) / eyeRadiusY;
            const float eyeShape = dx * dx + dy * dy;

            const float blinkPhase = std::fmod(static_cast<float>(timeMs), 3200.0f);
            const bool blinking = blinkPhase < 150.0f ||
                                  (blinkPhase > 2050.0f && blinkPhase < 2190.0f);
            const float pupilMotion = std::sin(static_cast<float>(timeMs) / 720.0f) * 0.22f;
            const float side = column < matrix.columns / 2 ? -1.0f : 1.0f;
            const float pupilX = dx - pupilMotion * side;
            const float pupilY = dy;
            const float pupilDistance = pupilX * pupilX + pupilY * pupilY;

            if (blinking) {
                intensity = std::fabs(dy) < 0.12f && eyeShape < 1.0f ? 0.8f : 0.0f;
            } else if (eyeShape <= 1.0f) {
                if (pupilDistance < 0.12f) {
                    red = 5; green = 8; blue = 18;
                    intensity = 1.0f;
                } else if (pupilDistance < 0.34f) {
                    red = 20; green = 155; blue = 255;
                    intensity = 1.0f;
                } else {
                    red = effect.red; green = effect.green; blue = effect.blue;
                    intensity = 0.85f;
                }
            } else {
                intensity = 0.0f;
            }
        } else {
            const bool firstHalf = column < matrix.columns / 2;
            const bool alternate =
                std::fmod(static_cast<float>(timeMs) / 180.0f, 1.0f) < 0.5f;
            intensity = (firstHalf == alternate) ? 1.0f : 0.0f;
        }
        break;
    }

    case LedEffectType::RunningText: {
        if (effect.text.empty() || matrix.rows < 7)
            return true;
        const float spacing = effect.textSpacing < 0.25f ? 0.25f : effect.textSpacing;
        const float charWidth = 6.0f * spacing;
        const float totalWidth = charWidth * static_cast<float>(effect.text.size());
        if (totalWidth <= 0.0f)
            return true;
        const float scroll = std::fmod(
            static_cast<float>(timeMs) * speed / 180.0f,
            totalWidth
        );
        float sample = std::fmod(static_cast<float>(column) + scroll, totalWidth);
        const size_t charIndex = static_cast<size_t>(sample / charWidth);
        const float within = sample - static_cast<float>(charIndex) * charWidth;
        const int glyphColumn = static_cast<int>(within / spacing);
        if (glyphColumn >= 0 && glyphColumn < 5) {
            const uint8_t bits = GetGlyphRow(effect.text[charIndex], row);
            intensity = (bits & (1u << (4 - glyphColumn))) ? 1.0f : 0.0f;
        } else {
            intensity = 0.0f;
        }
        red = effect.red;
        green = effect.green;
        blue = effect.blue;
        break;
    }

    case LedEffectType::RGB: {
        const float hue =
            static_cast<float>(timeMs) * 0.06f * speed +
            directionPosition * 360.0f;

        HSVtoRGB(hue, red, green, blue);
        break;
    }

    default:
        // CustomAnimation requires user-supplied animation frames.
        return false;
    }

    const float brightness =
        Clamp01(effect.brightness) *
        Clamp01(pixel->brightness);

    if (effect.colorMode == LedColorMode::Gradient) {
        const float blend = Clamp01(directionPosition);
        red = ClampColor(static_cast<float>(red) * (1.0f - blend) + effect.secondaryRed * blend);
        green = ClampColor(static_cast<float>(green) * (1.0f - blend) + effect.secondaryGreen * blend);
        blue = ClampColor(static_cast<float>(blue) * (1.0f - blend) + effect.secondaryBlue * blend);
    } else if (effect.colorMode == LedColorMode::Rainbow &&
               effect.type != LedEffectType::RGB) {
        const float hue = static_cast<float>(timeMs) * 0.06f * speed + directionPosition * 360.0f;
        HSVtoRGB(hue, red, green, blue);
    }

    output.red = red;
    output.green = green;
    output.blue = blue;
    output.intensity = Clamp01(intensity) * brightness;
    output.alpha = static_cast<uint8_t>(Clamp01(effect.opacity) * 255.0f);
    output.visible = output.intensity > 0.001f && output.alpha > 0;

    return true;
}

} // namespace VLS
