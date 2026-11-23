#include "matrix_animation_engine.h"
#include "led_motion_engine.h"
#include "profile_storage.h"

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>

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

bool ReadPpmToken(const std::string& data, size_t& position, std::string& token) {
    token.clear();
    while (position < data.size()) {
        if (std::isspace(static_cast<unsigned char>(data[position]))) { ++position; continue; }
        if (data[position] == '#') {
            while (position < data.size() && data[position] != '\n') ++position;
            continue;
        }
        break;
    }
    while (position < data.size() &&
           !std::isspace(static_cast<unsigned char>(data[position])) &&
           data[position] != '#') {
        token.push_back(data[position++]);
    }
    return !token.empty();
}

void ApplyCustomTexture(
    const LedEffectConfig& effect,
    const LedMatrixConfig& matrix,
    uint16_t column,
    uint16_t row,
    MatrixPixelOutput& output
) {
    if (effect.textureName.empty() || matrix.columns == 0 || matrix.rows == 0)
        return;
    static std::string cachedName;
    static uint8_t pixels[128 * 128 * 3] = {};
    static uint16_t width = 0;
    static uint16_t height = 0;
    static bool valid = false;
    if (cachedName != effect.textureName) {
        cachedName = effect.textureName;
        valid = false;
        width = height = 0;
        std::string data, token;
        if (ProfileStorage::LoadTexturePpm(effect.textureName, data)) {
            size_t pos = 0;
            if (ReadPpmToken(data, pos, token) && token == "P3" &&
                ReadPpmToken(data, pos, token)) {
                const int w = std::atoi(token.c_str());
                if (ReadPpmToken(data, pos, token)) {
                    const int h = std::atoi(token.c_str());
                    if (ReadPpmToken(data, pos, token)) {
                        const int maxValue = std::atoi(token.c_str());
                        if (w > 0 && w <= 128 && h > 0 && h <= 128 &&
                            maxValue > 0 && maxValue <= 255) {
                            bool ok = true;
                            for (int i = 0; i < w * h * 3; ++i) {
                                if (!ReadPpmToken(data, pos, token)) { ok = false; break; }
                                const int value = std::atoi(token.c_str());
                                if (value < 0 || value > maxValue) { ok = false; break; }
                                pixels[i] = static_cast<uint8_t>((value * 255) / maxValue);
                            }
                            if (ok) {
                                width = static_cast<uint16_t>(w);
                                height = static_cast<uint16_t>(h);
                                valid = true;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!valid || !output.visible)
        return;
    const uint16_t tx = static_cast<uint16_t>(
        (static_cast<uint32_t>(column) * width) / matrix.columns);
    const uint16_t ty = static_cast<uint16_t>(
        (static_cast<uint32_t>(row) * height) / matrix.rows);
    const size_t index = (static_cast<size_t>(ty) * width + tx) * 3;
    output.red = pixels[index];
    output.green = pixels[index + 1];
    output.blue = pixels[index + 2];
}

uint8_t GetGlyphRow(char input, uint16_t row, const std::string& fontName) {
    static std::string cachedFontName;
    static uint8_t customFont[95][7] = {};
    static bool customFontValid = false;
    if (fontName != cachedFontName) {
        cachedFontName = fontName;
        customFontValid = false;
        std::string bitmap;
        if (ProfileStorage::LoadFontBitmap(fontName, bitmap)) {
            std::string digits;
            digits.reserve(1330);
            for (char c : bitmap) {
                if (std::isspace(static_cast<unsigned char>(c))) continue;
                digits.push_back(c);
            }
            if (digits.size() == 1330) {
                bool valid = true;
                for (size_t glyph = 0; glyph < 95 && valid; ++glyph) {
                    for (size_t y = 0; y < 7; ++y) {
                        const size_t pos = (glyph * 7 + y) * 2;
                        unsigned int value = 0;
                        for (size_t k = 0; k < 2; ++k) {
                            const char ch = digits[pos + k];
                            value <<= 4;
                            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned int>(ch - '0');
                            else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned int>(ch - 'a' + 10);
                            else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned int>(ch - 'A' + 10);
                            else { valid = false; break; }
                        }
                        if (!valid || value > 31u) { valid = false; break; }
                        customFont[glyph][y] = static_cast<uint8_t>(value);
                    }
                }
                customFontValid = valid;
            }
        }
    }
    if (customFontValid && row < 7) {
        const unsigned char code = static_cast<unsigned char>(input);
        if (code >= 32 && code <= 126)
            return customFont[code - 32][row];
    }
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
    MatrixPixelOutput& output,
    MotionPlaybackMode playback,
    uint32_t motionIntervalMs
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
    const float colorPosition = GetColorPosition(
        effect.colorDirection, x, y, effect.colorSpread
    );

    // Motion is evaluated once below from the selected LedMotionPattern.
    // Do not retain the legacy per-effect directional sweep here.
    float intensity = 1.0f;

    uint8_t red = pixel->red;
    uint8_t green = pixel->green;
    uint8_t blue = pixel->blue;
    bool preserveEyeDetailColor = false;

    switch (effect.type) {
    case LedEffectType::RunningLED:
        // Spatial sweep comes exclusively from effect.motionPattern.
        intensity = 1.0f;
        break;

    case LedEffectType::StroboWave: {
        // Keep the strobe cadence, but remove its hidden LTR spatial head.
        const bool flashOn =
            std::fmod(static_cast<float>(timeMs) * speed / 250.0f, 1.0f)
            < 0.5f;
        intensity = flashOn ? 1.0f : 0.0f;
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
                    preserveEyeDetailColor = true;
                    red = 5; green = 8; blue = 18;
                    intensity = 1.0f;
                } else if (pupilDistance < 0.34f) {
                    preserveEyeDetailColor = true;
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
            // Narrow matrices cannot draw eye details; keep a simple lit field
            // until a dedicated compact-eye layout is selected.
            intensity = 1.0f;
        }
        break;
    }

    case LedEffectType::RunningText: {
        if (effect.text.empty() || matrix.rows < 7 || matrix.columns < 5)
            return true;
        const float spacing = effect.textSpacing < 0.25f ? 0.25f : effect.textSpacing;
        const float charWidth = 6.0f * spacing;
        const float totalWidth = charWidth * static_cast<float>(effect.text.size());
        if (totalWidth <= 0.0f)
            return true;

        const float elapsed = static_cast<float>(timeMs) * speed / 180.0f;
        const bool vertical = effect.textMotion == RunningTextMotion::ScrollUp ||
                              effect.textMotion == RunningTextMotion::ScrollDown ||
                              effect.textMotion == RunningTextMotion::BounceVertical;
        const float axisLength = vertical ? static_cast<float>(matrix.rows) : static_cast<float>(matrix.columns);
        const float contentLength = vertical ? totalWidth : totalWidth;
        float offset = 0.0f;
        bool reverse = false;

        switch (effect.textMotion) {
        case RunningTextMotion::ScrollRight:
        case RunningTextMotion::ScrollDown:
            offset = std::fmod(elapsed, contentLength + axisLength);
            break;
        case RunningTextMotion::BounceHorizontal:
        case RunningTextMotion::BounceVertical: {
            const float travel = contentLength + axisLength;
            const float cycle = std::fmod(elapsed, travel * 2.0f);
            reverse = cycle >= travel;
            offset = reverse ? (travel * 2.0f - cycle) : cycle;
            break;
        }
        case RunningTextMotion::Static:
            offset = (axisLength - contentLength) * 0.5f;
            break;
        default:
            offset = std::fmod(elapsed, contentLength + axisLength);
            break;
        }

        float sample = 0.0f;
        int glyphColumn = 0;
        uint16_t glyphRow = 0;
        if (!vertical) {
            float coordinate = static_cast<float>(column);
            if (effect.textMotion == RunningTextMotion::ScrollRight ||
                effect.textMotion == RunningTextMotion::BounceHorizontal ||
                effect.textMotion == RunningTextMotion::Static)
                coordinate -= offset;
            else
                coordinate += offset;
            if (effect.textMotion == RunningTextMotion::BounceHorizontal && reverse)
                coordinate = static_cast<float>(matrix.columns - 1) - coordinate;
            sample = totalWidth - 0.0001f - coordinate;
            if (sample >= 0.0f && sample < totalWidth) {
                const size_t charIndex = static_cast<size_t>(sample / charWidth);
                const float within = sample - static_cast<float>(charIndex) * charWidth;
                glyphColumn = static_cast<int>(within / spacing);
                glyphRow = row;
                if (glyphColumn >= 0 && glyphColumn < 5)
                    intensity = (GetGlyphRow(effect.text[charIndex], glyphRow, effect.fontName) & (1u << (4 - glyphColumn))) ? 1.0f : 0.0f;
                else
                    intensity = 0.0f;
            } else intensity = 0.0f;
        } else {
            float coordinate = static_cast<float>(row);
            if (effect.textMotion == RunningTextMotion::ScrollDown ||
                effect.textMotion == RunningTextMotion::BounceVertical)
                coordinate -= offset;
            else
                coordinate += offset;
            if (effect.textMotion == RunningTextMotion::BounceVertical && reverse)
                coordinate = static_cast<float>(matrix.rows - 1) - coordinate;
            sample = totalWidth - 0.0001f - coordinate;
            if (sample >= 0.0f && sample < totalWidth) {
                const size_t charIndex = static_cast<size_t>(sample / charWidth);
                const float within = sample - static_cast<float>(charIndex) * charWidth;
                const int glyphRow = static_cast<int>(within / spacing);
                if (column < 7 && glyphRow >= 0 && glyphRow < 5) {
                    const uint8_t bits = GetGlyphRow(effect.text[charIndex], column, effect.fontName);
                    intensity = (bits & (1u << (4 - glyphRow))) ? 1.0f : 0.0f;
                } else intensity = 0.0f;
            } else intensity = 0.0f;
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

    // Only the three general-purpose LED effects share this motion field.
    // Eyes and text have their own geometry, timing and movement systems.
    if (effect.type != LedEffectType::StroboEyes &&
        effect.type != LedEffectType::RunningText) {
        const float phase = Fract(static_cast<float>(timeMs) * speed / 1800.0f);
        intensity *= EvaluateMotionIntensity(
            ResolveMotionPattern(effect, timeMs, playback, motionIntervalMs), x, y, phase,
            column, row, matrix.columns, matrix.rows
        );
    }

    const float brightness =
        Clamp01(effect.brightness) *
        Clamp01(pixel->brightness);

    if (effect.type != LedEffectType::RGB && !preserveEyeDetailColor) {
        const float spread = effect.colorSpread < 0.1f ? 0.1f : effect.colorSpread;
        const float rawPosition = colorPosition * spread;
        const float blend = rawPosition - std::floor(rawPosition);
        switch (effect.colorMode) {
        case LedColorMode::Gradient:
            red = ClampColor(static_cast<float>(red) * (1.0f - blend) + effect.secondaryRed * blend);
            green = ClampColor(static_cast<float>(green) * (1.0f - blend) + effect.secondaryGreen * blend);
            blue = ClampColor(static_cast<float>(blue) * (1.0f - blend) + effect.secondaryBlue * blend);
            break;
        case LedColorMode::ThreeColorGradient: {
            const float localBlend = blend < 0.5f ? blend * 2.0f : (blend - 0.5f) * 2.0f;
            const float startR = blend < 0.5f ? effect.red : effect.secondaryRed;
            const float startG = blend < 0.5f ? effect.green : effect.secondaryGreen;
            const float startB = blend < 0.5f ? effect.blue : effect.secondaryBlue;
            const float endR = blend < 0.5f ? effect.secondaryRed : effect.tertiaryRed;
            const float endG = blend < 0.5f ? effect.secondaryGreen : effect.tertiaryGreen;
            const float endB = blend < 0.5f ? effect.secondaryBlue : effect.tertiaryBlue;
            red = ClampColor(startR * (1.0f - localBlend) + endR * localBlend);
            green = ClampColor(startG * (1.0f - localBlend) + endG * localBlend);
            blue = ClampColor(startB * (1.0f - localBlend) + endB * localBlend);
            break;
        }
        case LedColorMode::Rainbow: {
            const float hue = static_cast<float>(timeMs) * 0.06f * speed + blend * 360.0f;
            HSVtoRGB(hue, red, green, blue);
            break;
        }
        case LedColorMode::ColorCycle:
            HSVtoRGB(static_cast<float>(timeMs) * 0.06f * speed, red, green, blue);
            break;
        case LedColorMode::ColorWave: {
            const float wave = 0.5f + 0.5f * std::sin(
                blend * TWO_PI * 2.0f - static_cast<float>(timeMs) * 0.004f * speed
            );
            HSVtoRGB(wave * 360.0f + static_cast<float>(timeMs) * 0.02f * speed, red, green, blue);
            break;
        }
        case LedColorMode::ColorPulse: {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(timeMs) * 0.006f * speed);
            red = ClampColor(static_cast<float>(red) * (1.0f - pulse) + effect.secondaryRed * pulse);
            green = ClampColor(static_cast<float>(green) * (1.0f - pulse) + effect.secondaryGreen * pulse);
            blue = ClampColor(static_cast<float>(blue) * (1.0f - pulse) + effect.secondaryBlue * pulse);
            break;
        }
        case LedColorMode::Alternating: {
            const uint32_t band = static_cast<uint32_t>(
                std::floor(blend * 8.0f + static_cast<float>(timeMs) * 0.002f * speed)
            );
            if ((band & 1u) != 0u) {
                red = effect.secondaryRed; green = effect.secondaryGreen; blue = effect.secondaryBlue;
            }
            break;
        }
        case LedColorMode::Solid:
        default:
            break;
        }
    }

    output.red = red;
    output.green = green;
    output.blue = blue;
    output.intensity = Clamp01(intensity) * brightness;
    output.alpha = static_cast<uint8_t>(Clamp01(effect.opacity) * 255.0f);
    output.visible = output.intensity > 0.001f && output.alpha > 0;
    ApplyCustomTexture(effect, matrix, column, row, output);

    return true;
}

} // namespace VLS
