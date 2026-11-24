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

struct TextureCacheEntry {
    std::string name;
    uint8_t pixels[128 * 128 * 3] = {};
    uint16_t width = 0;
    uint16_t height = 0;
    bool valid = false;
};

void ApplyCustomTexture(
    const LedEffectConfig& effect,
    const LedMatrixConfig& matrix,
    uint16_t column,
    uint16_t row,
    MatrixPixelOutput& output
) {
    if (effect.textureName.empty() || matrix.columns == 0 ||
        matrix.rows == 0 || !output.visible)
        return;

    // Four small fixed cache slots allow simultaneous effects to retain
    // different textures without allocating memory during pixel rendering.
    static TextureCacheEntry cache[4];
    static uint32_t nextSlot = 0;
    TextureCacheEntry* entry = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        if (cache[i].name == effect.textureName) {
            entry = &cache[i];
            break;
        }
    }
    if (entry == nullptr) {
        entry = &cache[nextSlot++ % 4u];
        entry->name = effect.textureName;
        entry->valid = false;
        entry->width = entry->height = 0;

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
                                if (!ReadPpmToken(data, pos, token)) {
                                    ok = false;
                                    break;
                                }
                                const int value = std::atoi(token.c_str());
                                if (value < 0 || value > maxValue) {
                                    ok = false;
                                    break;
                                }
                                entry->pixels[i] =
                                    static_cast<uint8_t>((value * 255) / maxValue);
                            }
                            if (ok) {
                                entry->width = static_cast<uint16_t>(w);
                                entry->height = static_cast<uint16_t>(h);
                                entry->valid = true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!entry->valid)
        return;
    const uint16_t tx = static_cast<uint16_t>(
        (static_cast<uint32_t>(column) * entry->width) / matrix.columns);
    const uint16_t ty = static_cast<uint16_t>(
        (static_cast<uint32_t>(row) * entry->height) / matrix.rows);
    const size_t index =
        (static_cast<size_t>(ty) * entry->width + tx) * 3;
    output.red = entry->pixels[index];
    output.green = entry->pixels[index + 1];
    output.blue = entry->pixels[index + 2];
}

struct FontCacheEntry {
    std::string name;
    uint8_t glyphs[95][7] = {};
    bool valid = false;
};

uint8_t GetGlyphRow(
    char input,
    uint16_t row,
    const std::string& fontName
) {
    static FontCacheEntry cache[4];
    static uint32_t nextSlot = 0;
    FontCacheEntry* entry = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        if (cache[i].name == fontName) {
            entry = &cache[i];
            break;
        }
    }
    if (entry == nullptr) {
        entry = &cache[nextSlot++ % 4u];
        entry->name = fontName;
        entry->valid = false;
        std::string bitmap;
        if (ProfileStorage::LoadFontBitmap(fontName, bitmap)) {
            std::string digits;
            digits.reserve(1330);
            for (char c : bitmap) {
                if (!std::isspace(static_cast<unsigned char>(c)))
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
                            if (ch >= '0' && ch <= '9')
                                value |= static_cast<unsigned int>(ch - '0');
                            else if (ch >= 'a' && ch <= 'f')
                                value |= static_cast<unsigned int>(ch - 'a' + 10);
                            else if (ch >= 'A' && ch <= 'F')
                                value |= static_cast<unsigned int>(ch - 'A' + 10);
                            else {
                                valid = false;
                                break;
                            }
                        }
                        if (!valid || value > 31u) {
                            valid = false;
                            break;
                        }
                        entry->glyphs[glyph][y] =
                            static_cast<uint8_t>(value);
                    }
                }
                entry->valid = valid;
            }
        }
    }

    if (entry->valid && row < 7) {
        const unsigned char code = static_cast<unsigned char>(input);
        if (code >= 32 && code <= 126)
            return entry->glyphs[code - 32][row];
    }
    if (row >= 7)
        return 0;

    const char ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(input))
    );
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

EyeMotion ResolveEyeMotion(
    const LedEffectConfig& effect,
    uint32_t timeMs
) {
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Manual)
        return effect.eyeMotion;
    const uint32_t interval = effect.eyeMotionIntervalMs < 500
        ? 500 : effect.eyeMotionIntervalMs;
    const uint32_t step = timeMs / interval;
    static const EyeMotion automaticOrder[] = {
        EyeMotion::Idle, EyeMotion::Scan, EyeMotion::LookLeft,
        EyeMotion::LookRight, EyeMotion::LookUp, EyeMotion::LookDown,
        EyeMotion::Shake, EyeMotion::Wink, EyeMotion::RapidBlink
    };
    constexpr uint32_t count = static_cast<uint32_t>(EyeMotion::RapidBlink) + 1u;
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Automatic)
        return automaticOrder[step % (sizeof(automaticOrder) / sizeof(automaticOrder[0]))];
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Sequential)
        return static_cast<EyeMotion>(step % count);
    uint32_t value = step ^ 0x9E3779B9u;
    value ^= value >> 16; value *= 0x7FEB352Du;
    value ^= value >> 15; value *= 0x846CA68Bu;
    value ^= value >> 16;
    return static_cast<EyeMotion>(value % count);
}

EyeExpression ResolveEyeExpression(
    const LedEffectConfig& effect,
    uint32_t timeMs
) {
    if (effect.eyeExpressionPlayback == MotionPlaybackMode::Manual)
        return effect.eyeExpression;
    const uint32_t interval = effect.eyeExpressionIntervalMs < 500
        ? 500 : effect.eyeExpressionIntervalMs;
    const uint32_t step = timeMs / interval;
    static const EyeExpression automaticOrder[] = {
        EyeExpression::Neutral, EyeExpression::Happy,
        EyeExpression::Surprised, EyeExpression::Angry,
        EyeExpression::Evil, EyeExpression::Sad,
        EyeExpression::Sleepy, EyeExpression::Love
    };
    constexpr uint32_t count = static_cast<uint32_t>(EyeExpression::Evil) + 1u;
    if (effect.eyeExpressionPlayback == MotionPlaybackMode::Automatic)
        return automaticOrder[step % (sizeof(automaticOrder) / sizeof(automaticOrder[0]))];
    if (effect.eyeExpressionPlayback == MotionPlaybackMode::Sequential)
        return static_cast<EyeExpression>(step % count);
    uint32_t value = step ^ 0x85EBCA6Bu;
    value ^= value >> 16; value *= 0x7FEB352Du;
    value ^= value >> 15; value *= 0x846CA68Bu;
    value ^= value >> 16;
    return static_cast<EyeExpression>(value % count);
}

EyeVisualStyle ResolveEyeVisualStyle(
    const LedEffectConfig& effect,
    uint32_t timeMs
) {
    if (!effect.eyeTransformationEnabled)
        return effect.eyeVisualStyle;
    const uint32_t interval = effect.eyeTransformationIntervalMs < 1000
        ? 1000 : effect.eyeTransformationIntervalMs;
    return static_cast<EyeVisualStyle>((timeMs / interval) % 4u);
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
            const bool leftEye = column < matrix.columns / 2;
            const float eyeCenterX = leftEye
                ? static_cast<float>(matrix.columns) * 0.25f
                : static_cast<float>(matrix.columns) * 0.75f;
            const float eyeCenterY = static_cast<float>(matrix.rows - 1) * 0.5f;
            const float eyeRadiusX = static_cast<float>(matrix.columns) * 0.19f;
            float eyeRadiusY = static_cast<float>(matrix.rows) * 0.43f;
            const float dx = (static_cast<float>(column) - eyeCenterX) / eyeRadiusX;
            const float dy = (static_cast<float>(row) - eyeCenterY) / eyeRadiusY;
            const float inwardX = dx * (leftEye ? 1.0f : -1.0f);
            const float t = static_cast<float>(timeMs) * speed;
            const EyeMotion activeMotion = ResolveEyeMotion(effect, timeMs);
            const EyeExpression activeExpression = ResolveEyeExpression(effect, timeMs);
            const EyeVisualStyle activeVisualStyle = ResolveEyeVisualStyle(effect, timeMs);

            if (activeExpression == EyeExpression::Surprised)
                eyeRadiusY *= 1.14f;

            float verticalScale = 1.0f;
            switch (effect.eyeShape) {
            case EyeShape::Narrow: verticalScale = 0.58f; break;
            case EyeShape::Wide: verticalScale = 1.18f; break;
            case EyeShape::Round: verticalScale = 0.92f; break;
            case EyeShape::Cat: verticalScale = 0.72f; break;
            case EyeShape::Square: verticalScale = 0.82f; break;
            case EyeShape::Classic:
            default: break;
            }

            float topLimit = -1.0f;
            float bottomLimit = 1.0f;
            switch (activeExpression) {
            case EyeExpression::Happy:
                topLimit = -0.88f + 0.38f * std::fabs(dx);
                bottomLimit = 0.88f - 0.38f * std::fabs(dx);
                break;
            case EyeExpression::Angry:
            case EyeExpression::Evil:
                topLimit = -0.88f + 0.42f * inwardX;
                bottomLimit = 0.92f;
                verticalScale *= 0.88f;
                break;
            case EyeExpression::Sad:
                topLimit = -0.72f - 0.12f * inwardX;
                bottomLimit = 0.88f;
                break;
            case EyeExpression::Sleepy:
                topLimit = -0.12f;
                bottomLimit = 0.92f;
                verticalScale *= 0.72f;
                break;
            case EyeExpression::Surprised:
                verticalScale *= 1.12f;
                break;
            case EyeExpression::Neutral:
            case EyeExpression::Love:
            default:
                break;
            }

            const float scaledY = dy / verticalScale;
            const float absX = std::fabs(dx);
            const float absY = std::fabs(scaledY);
            bool shapeInside = false;
            switch (effect.eyeShape) {
            case EyeShape::Cat: {
                // Almond silhouette: both corners taper instead of using a plain oval.
                const float almondHalfHeight = 0.94f - 0.48f * absX;
                shapeInside = absX <= 1.0f && absY <= almondHalfHeight;
                break;
            }
            case EyeShape::Square: {
                // Squared eye with clipped corners, suitable for coarse LED grids.
                const float cornerX = absX - 0.70f;
                const float cornerY = absY - 0.70f;
                shapeInside = absX <= 0.96f && absY <= 0.96f &&
                    (cornerX <= 0.0f || cornerY <= 0.0f ||
                     cornerX * cornerX + cornerY * cornerY <= 0.26f * 0.26f);
                break;
            }
            case EyeShape::Round:
                shapeInside = dx * dx + scaledY * scaledY <= 0.86f;
                break;
            case EyeShape::Classic:
            case EyeShape::Narrow:
            case EyeShape::Wide:
            default:
                shapeInside = dx * dx + scaledY * scaledY <= 1.0f;
                break;
            }
            const bool insideEye = shapeInside &&
                                   dy >= topLimit && dy <= bottomLimit;

            const uint32_t blinkInterval =
                effect.eyeBlinkIntervalMs < 500 ? 500 : effect.eyeBlinkIntervalMs;
            const float blinkPhase = std::fmod(t, static_cast<float>(blinkInterval));
            bool blinking = blinkPhase < 135.0f ||
                (blinkPhase > static_cast<float>(blinkInterval) * 0.68f &&
                 blinkPhase < static_cast<float>(blinkInterval) * 0.68f + 115.0f);
            if (effect.eyeTransformationEnabled) {
                const uint32_t transformInterval = effect.eyeTransformationIntervalMs < 1000
                    ? 1000 : effect.eyeTransformationIntervalMs;
                blinking = blinking ||
                    (timeMs % transformInterval) < 105u;
            }

            float gazeX = std::sin(t / 720.0f) * 0.10f;
            float gazeY = std::sin(t / 1050.0f) * 0.035f;
            switch (activeMotion) {
            case EyeMotion::LookLeft: gazeX = -0.34f; break;
            case EyeMotion::LookRight: gazeX = 0.34f; break;
            case EyeMotion::LookUp: gazeY = -0.34f; break;
            case EyeMotion::LookDown: gazeY = 0.34f; break;
            case EyeMotion::Scan: gazeX = std::sin(t / 390.0f) * 0.36f; break;
            case EyeMotion::Wink: {
                // Alternate the wink so both eyes take turns; keep normal
                // automatic blinking independent of the intentional wink.
                const uint32_t winkCycle = static_cast<uint32_t>(
                    std::fmod(t, 4800.0f)
                );
                const bool winkLeft = winkCycle < 420u;
                const bool winkRight = winkCycle >= 2400u &&
                                       winkCycle < 2820u;
                blinking = blinking || (leftEye ? winkLeft : winkRight);
                break;
            }
            case EyeMotion::Shake: gazeX = std::sin(t / 85.0f) * 0.30f; break;
            case EyeMotion::RapidBlink:
                blinking = std::fmod(t, 720.0f) < 115.0f;
                break;
            case EyeMotion::Idle:
            default:
                break;
            }

            const float pupilX = dx - gazeX;
            const float pupilY = dy - gazeY;
            const float pupilDistance = pupilX * pupilX + pupilY * pupilY;
            float pupilRadius = activeExpression == EyeExpression::Surprised
                ? 0.075f : 0.12f;
            if (activeExpression == EyeExpression::Love)
                pupilRadius = 0.16f;
            // Implicit heart equation gives Love a recognisable heart pupil
            // instead of relying only on a pink circular iris.
            const float heartX = pupilX * 2.8f;
            const float heartY = -pupilY * 2.8f;
            const float heartTerm = heartX * heartX +
                                    heartY * heartY - 1.0f;
            const bool heartPupil =
                heartTerm * heartTerm * heartTerm -
                heartX * heartX * heartY * heartY * heartY <= 0.0f;

            float browThickness = 0.0f;
            switch (effect.eyeBrowStyle) {
            case EyeBrowStyle::Natural: browThickness = 0.105f; break;
            case EyeBrowStyle::Sharp: browThickness = 0.14f; break;
            case EyeBrowStyle::Thick: browThickness = 0.22f; break;
            case EyeBrowStyle::None:
            default: break;
            }
            float browTilt = effect.eyeBrowTilt * 0.18f;
            if (activeExpression == EyeExpression::Angry ||
                activeExpression == EyeExpression::Evil)
                browTilt += 0.16f;
            const float browLine = -1.04f + inwardX * browTilt;
            const bool browPixel = effect.eyeBrowStyle != EyeBrowStyle::None &&
                matrix.rows >= 10 && absX < 0.88f &&
                std::fabs(dy - browLine) <= browThickness;

            if (browPixel) {
                red = effect.eyeBrowRed;
                green = effect.eyeBrowGreen;
                blue = effect.eyeBrowBlue;
                intensity = 1.0f;
            } else if (blinking) {
                intensity = insideEye && std::fabs(dy - 0.05f) < 0.10f
                    ? 0.9f : 0.0f;
            } else if (insideEye) {
                if ((activeExpression == EyeExpression::Love && heartPupil) ||
                    (activeExpression != EyeExpression::Love &&
                     pupilDistance < pupilRadius)) {
                    preserveEyeDetailColor = true;
                    if (activeExpression == EyeExpression::Love) {
                        red = 255; green = 70; blue = 150;
                    } else {
                        red = effect.eyePupilRed;
                        green = effect.eyePupilGreen;
                        blue = effect.eyePupilBlue;
                    }
                    intensity = 1.0f;
                } else if (pupilDistance < 0.34f) {
                    preserveEyeDetailColor = true;
                    if (activeExpression == EyeExpression::Love) {
                        red = 255; green = 110; blue = 180;
                    } else if (activeVisualStyle == EyeVisualStyle::Normal) {
                        red = effect.eyeIrisRed;
                        green = effect.eyeIrisGreen;
                        blue = effect.eyeIrisBlue;
                    } else {
                        const float irisRadius = std::sqrt(pupilDistance);
                        const float irisAngle = std::atan2(pupilY, pupilX);
                        if (activeVisualStyle == EyeVisualStyle::Sharingan) {
                            const float tomoe = std::cos(irisAngle * 3.0f + 0.35f);
                            if (irisRadius > 0.19f && irisRadius < 0.30f && tomoe > 0.91f) {
                                red = 18; green = 2; blue = 8;
                            } else if (std::fabs(irisRadius - 0.245f) < 0.025f) {
                                red = 255; green = 75; blue = 55;
                            } else {
                                red = 190; green = 18; blue = 38;
                            }
                        } else if (activeVisualStyle == EyeVisualStyle::Mangekyou) {
                            const float spoke = std::fabs(std::cos(irisAngle * 3.0f + t / 1800.0f));
                            if (spoke > 0.88f && irisRadius > 0.12f) {
                                red = 20; green = 1; blue = 12;
                            } else {
                                red = 220; green = 28; blue = 48;
                            }
                        } else {
                            const float ring = std::fabs(std::sin(irisRadius * 42.0f));
                            if (ring > 0.82f) {
                                red = 225; green = 190; blue = 255;
                            } else {
                                red = 135; green = 82; blue = 220;
                            }
                        }
                    }
                    intensity = 1.0f;
                } else {
                    red = effect.red; green = effect.green; blue = effect.blue;
                    intensity = 0.85f;
                }
            } else {
                intensity = 0.0f;
            }
        } else {
            // Small matrices fall back to a simple blink field.
            const float blinkPhase = std::fmod(
                static_cast<float>(timeMs) * speed,
                static_cast<float>(effect.eyeBlinkIntervalMs < 500 ? 500 : effect.eyeBlinkIntervalMs)
            );
            intensity = blinkPhase < 140.0f ? 0.0f : 1.0f;
        }
        break;
    }

    case LedEffectType::RunningText: {
        if (effect.text.empty() || matrix.rows < 7 || matrix.columns < 5)
            return true;
        const float spacing = effect.textSpacing < 0.25f ? 0.25f : effect.textSpacing;
        const bool vertical =
            effect.textMotion == RunningTextMotion::ScrollUp ||
            effect.textMotion == RunningTextMotion::ScrollDown ||
            effect.textMotion == RunningTextMotion::BounceVertical;
        const float charWidth = 6.0f * spacing;
        // Vertical text stacks complete, upright 5x7 glyphs along the
        // scrolling axis. Each character occupies seven rows plus spacing.
        const float totalWidth = vertical
            ? 8.0f * spacing * static_cast<float>(effect.text.size())
            : charWidth * static_cast<float>(effect.text.size());
        if (totalWidth <= 0.0f)
            return true;

        const float elapsed = static_cast<float>(timeMs) * speed / 180.0f;
        const float axisLength = vertical
            ? static_cast<float>(matrix.rows)
            : static_cast<float>(matrix.columns);
        const float travel = totalWidth + axisLength;
        float textOrigin = 0.0f;

        switch (effect.textMotion) {
        case RunningTextMotion::ScrollRight:
            // Enter from the left and move toward the right.
            textOrigin = -totalWidth + std::fmod(elapsed, travel);
            break;
        case RunningTextMotion::ScrollDown:
            // Vertical equivalent of ScrollRight.
            textOrigin = -totalWidth + std::fmod(elapsed, travel);
            break;
        case RunningTextMotion::BounceHorizontal:
        case RunningTextMotion::BounceVertical: {
            const float cycle = std::fmod(elapsed, travel * 2.0f);
            textOrigin = cycle < travel
                ? axisLength - cycle
                : -totalWidth + (cycle - travel);
            break;
        }
        case RunningTextMotion::Static:
            textOrigin = (axisLength - totalWidth) * 0.5f;
            break;
        case RunningTextMotion::ScrollLeft:
        case RunningTextMotion::ScrollUp:
        default:
            // Enter from the far edge and move across the matrix.
            textOrigin = axisLength - std::fmod(elapsed, travel);
            break;
        }

        if (!vertical) {
            // Normal, readable 5x7 glyph orientation. Character order and
            // bit order both run left-to-right; only the text origin moves.
            const float local = static_cast<float>(column) - textOrigin;
            if (local >= 0.0f && local < totalWidth) {
                const size_t charIndex =
                    static_cast<size_t>(local / charWidth);
                const float within =
                    local - static_cast<float>(charIndex) * charWidth;
                const int glyphColumn =
                    static_cast<int>(within / spacing);
                if (charIndex < effect.text.size() &&
                    glyphColumn >= 0 && glyphColumn < 5) {
                    const uint8_t bits = GetGlyphRow(
                        effect.text[charIndex], row, effect.fontName
                    );
                    intensity = (bits & (1u << (4 - glyphColumn)))
                        ? 1.0f : 0.0f;
                } else {
                    intensity = 0.0f;
                }
            } else {
                intensity = 0.0f;
            }
        } else {
            // Keep each 5x7 glyph upright. Characters are stacked vertically
            // and the complete message moves as one column of text.
            const float local = static_cast<float>(row) - textOrigin;
            if (local >= 0.0f && local < totalWidth) {
                const float glyphHeight = 8.0f * spacing;
                const size_t charIndex =
                    static_cast<size_t>(local / glyphHeight);
                const float within =
                    local - static_cast<float>(charIndex) * glyphHeight;
                const int glyphRow =
                    static_cast<int>(within / spacing);
                const int glyphColumn =
                    static_cast<int>(static_cast<float>(column) / spacing);
                if (charIndex < effect.text.size() &&
                    glyphColumn >= 0 && glyphColumn < 5 &&
                    glyphRow >= 0 && glyphRow < 7) {
                    const uint8_t bits = GetGlyphRow(
                        effect.text[charIndex],
                        static_cast<uint16_t>(glyphRow),
                        effect.fontName
                    );
                    intensity = (bits & (1u << (4 - glyphColumn)))
                        ? 1.0f : 0.0f;
                } else {
                    intensity = 0.0f;
                }
            } else {
                intensity = 0.0f;
            }
        }
        red = effect.red;
        green = effect.green;
        blue = effect.blue;
        break;
    }

    case LedEffectType::RGB: {
        const float hue =
            static_cast<float>(timeMs) * 0.06f * speed +
            colorPosition * effect.colorSpread * 360.0f;

        HSVtoRGB(hue, red, green, blue);
        break;
    }

    default:
        // CustomAnimation requires user-supplied animation frames.
        return false;
    }

    // General LED effects use the shared motion field. Running Text keeps
    // its own scrolling engine, but can optionally receive a laser write/weld pass.
    const LedMotionPattern activePattern =
        ResolveMotionPattern(effect, timeMs, playback, motionIntervalMs);
    if (effect.type != LedEffectType::StroboEyes &&
        (effect.type != LedEffectType::RunningText ||
         activePattern == LedMotionPattern::LaserWrite ||
         activePattern == LedMotionPattern::LaserWeld)) {
        const float phase = Fract(static_cast<float>(timeMs) * speed / 1800.0f);
        const bool verticalText =
            effect.textMotion == RunningTextMotion::ScrollUp ||
            effect.textMotion == RunningTextMotion::ScrollDown ||
            effect.textMotion == RunningTextMotion::BounceVertical;
        const float motionX = effect.type == LedEffectType::RunningText && verticalText ? y : x;
        const float motionY = effect.type == LedEffectType::RunningText && verticalText ? x : y;
        intensity *= EvaluateMotionIntensity(
            activePattern, motionX, motionY, phase,
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
