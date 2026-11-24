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


bool IsGlyphPixelCovered(
    char character,
    float pixelLeft,
    float pixelTop,
    float glyphStep,
    const std::string& fontName
) {
    if (glyphStep <= 0.0f)
        return false;

    const float pixelRight = pixelLeft + 1.0f;
    const float pixelBottom = pixelTop + 1.0f;
    const float epsilon = 0.0001f;

    int firstColumn = static_cast<int>(std::floor(pixelLeft / glyphStep));
    int lastColumn = static_cast<int>(std::floor((pixelRight - epsilon) / glyphStep));
    int firstRow = static_cast<int>(std::floor(pixelTop / glyphStep));
    int lastRow = static_cast<int>(std::floor((pixelBottom - epsilon) / glyphStep));

    if (firstColumn < 0) firstColumn = 0;
    if (firstRow < 0) firstRow = 0;
    if (lastColumn > 4) lastColumn = 4;
    if (lastRow > 6) lastRow = 6;
    if (firstColumn > 4 || firstRow > 6 || lastColumn < 0 || lastRow < 0)
        return false;

    // Area-aware nearest-cell sampling: when glyph cells are smaller than one
    // LED pixel, retain any lit source cell covered by that output pixel.
    for (int glyphRow = firstRow; glyphRow <= lastRow; ++glyphRow) {
        const float rowTop = static_cast<float>(glyphRow) * glyphStep;
        const float rowBottom = rowTop + glyphStep;
        if (rowBottom <= pixelTop || rowTop >= pixelBottom)
            continue;
        const uint8_t bits = GetGlyphRow(
            character, static_cast<uint16_t>(glyphRow), fontName
        );
        for (int glyphColumn = firstColumn; glyphColumn <= lastColumn; ++glyphColumn) {
            const float columnLeft = static_cast<float>(glyphColumn) * glyphStep;
            const float columnRight = columnLeft + glyphStep;
            if (columnRight <= pixelLeft || columnLeft >= pixelRight)
                continue;
            if ((bits & (1u << (4 - glyphColumn))) != 0u)
                return true;
        }
    }
    return false;
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
        EyeMotion::Shake, EyeMotion::Wink, EyeMotion::RapidBlink,
        EyeMotion::Sharingan, EyeMotion::Mangekyou, EyeMotion::Rinnegan
    };
    constexpr uint32_t count = static_cast<uint32_t>(EyeMotion::Rinnegan) + 1u;
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
    uint32_t motionIntervalMs,
    const MotionFrameState* motionFrame
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
    const MotionFrameState localMotionFrame = motionFrame != nullptr
        ? *motionFrame
        : BuildMotionFrameState(effect, timeMs, playback, motionIntervalMs);
    const float colorPosition = GetColorPosition(
        effect.colorDirection, x, y, effect.colorSpread
    );
    // Color and brightness sample the same placement-level phase.
    // Per-pixel speed must not create a second clock that drifts from the
    // shared pattern/background field.
    const float colorPhase = localMotionFrame.phase;

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
        const bool flashOn = localMotionFrame.phase < 0.5f;
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
            // Eye Expression is retired: shape controls the eyelid silhouette,
            // while Eye Motion owns gaze, blinking and the three iris motifs.
            const EyeExpression activeExpression = EyeExpression::Neutral;
            const EyeVisualStyle activeVisualStyle =
                activeMotion == EyeMotion::Sharingan ? EyeVisualStyle::Sharingan :
                activeMotion == EyeMotion::Mangekyou ? EyeVisualStyle::Mangekyou :
                activeMotion == EyeMotion::Rinnegan ? EyeVisualStyle::Rinnegan :
                EyeVisualStyle::Normal;

            if (activeExpression == EyeExpression::Surprised)
                eyeRadiusY *= 1.14f;

            float verticalScale = 1.0f;
            switch (effect.eyeShape) {
            case EyeShape::Narrow: verticalScale = 0.58f; break;
            case EyeShape::Wide: verticalScale = 1.18f; break;
            case EyeShape::Round: verticalScale = 0.92f; break;
            case EyeShape::Cat: verticalScale = 0.72f; break;
            case EyeShape::Square: verticalScale = 0.82f; break;
            case EyeShape::Sleep: verticalScale = 0.52f; break;
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
            // Motif selection is now part of Eye Motion. Do not let the legacy
            // timed transformation setting replace the selected motif.

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
            case EyeMotion::Sharingan:
            case EyeMotion::Mangekyou:
            case EyeMotion::Rinnegan:
                // Motifs alter iris artwork only; pupil gaze remains independent.
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
                preserveEyeDetailColor = true;
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
                    const float irisRadius = std::sqrt(pupilDistance);
                    const float irisAngle = std::atan2(pupilY, pupilX);
                    const float irisEdge = Clamp01((0.34f - irisRadius) * 18.0f);
                    const bool outerIrisRing = irisRadius > 0.305f;
                    const bool innerIrisRing = std::fabs(irisRadius - 0.155f) < 0.018f;

                    if (activeVisualStyle == EyeVisualStyle::Normal) {
                        // Radial fibers and two concentric rings give the
                        // selected iris palette depth without image assets.
                        const float fiber = 0.82f + 0.18f *
                            std::fabs(std::sin(irisAngle * 17.0f + irisRadius * 46.0f));
                        const float radial = 0.78f + 0.22f * irisEdge;
                        if (outerIrisRing) {
                            red = static_cast<uint8_t>(effect.eyePupilRed);
                            green = static_cast<uint8_t>(effect.eyePupilGreen);
                            blue = static_cast<uint8_t>(effect.eyePupilBlue);
                        } else if (innerIrisRing) {
                            red = ClampColor(effect.eyeIrisRed * 1.18f);
                            green = ClampColor(effect.eyeIrisGreen * 1.18f);
                            blue = ClampColor(effect.eyeIrisBlue * 1.18f);
                        } else {
                            red = ClampColor(effect.eyeIrisRed * fiber * radial);
                            green = ClampColor(effect.eyeIrisGreen * fiber * radial);
                            blue = ClampColor(effect.eyeIrisBlue * fiber * radial);
                        }
                    } else if (activeVisualStyle == EyeVisualStyle::Sharingan) {
                        const float tomoe = std::cos(irisAngle * 3.0f + 0.35f);
                        const bool tomoeMark = irisRadius > 0.205f &&
                            irisRadius < 0.292f && tomoe > 0.88f;
                        if (outerIrisRing || tomoeMark) {
                            red = 16; green = 2; blue = 8;
                        } else if (innerIrisRing) {
                            red = 255; green = 82; blue = 62;
                        } else {
                            const float ember = 0.82f + 0.18f *
                                std::fabs(std::sin(irisAngle * 9.0f + irisRadius * 34.0f));
                            red = ClampColor(205.0f * ember);
                            green = ClampColor(20.0f * ember);
                            blue = ClampColor(42.0f * ember);
                        }
                    } else if (activeVisualStyle == EyeVisualStyle::Mangekyou) {
                        const float rotation = t / 1450.0f;
                        const float petal = std::fabs(std::cos(
                            irisAngle * 3.0f + rotation + irisRadius * 5.0f));
                        const bool darkSpoke = petal > 0.82f && irisRadius > 0.145f;
                        if (outerIrisRing || darkSpoke) {
                            red = 22; green = 1; blue = 13;
                        } else if (innerIrisRing) {
                            red = 255; green = 68; blue = 76;
                        } else {
                            const float glow = 0.78f + 0.22f *
                                (0.5f + 0.5f * std::sin(irisRadius * 30.0f - rotation));
                            red = ClampColor(225.0f * glow);
                            green = ClampColor(24.0f * glow);
                            blue = ClampColor(55.0f * glow);
                        }
                    } else {
                        // Rinnegan: concentric violet rings with a slowly
                        // travelling ripple and a bright outer limbal edge.
                        const float rings = std::fabs(std::sin(
                            irisRadius * 68.0f - std::sin(irisAngle * 6.0f + t / 2100.0f) * 0.45f
                        ));
                        if (outerIrisRing || rings > 0.84f) {
                            red = 224; green = 194; blue = 255;
                        } else if (innerIrisRing) {
                            red = 178; green = 128; blue = 255;
                        } else {
                            red = ClampColor(118.0f + 35.0f * irisEdge);
                            green = ClampColor(62.0f + 24.0f * irisEdge);
                            blue = ClampColor(218.0f + 32.0f * irisEdge);
                        }
                    }

                    // Tiny reflected catchlights make the iris read as a
                    // glossy surface rather than a flat block of LEDs.
                    const float glintX = pupilX + 0.145f;
                    const float glintY = pupilY + 0.135f;
                    if (glintX * glintX + glintY * glintY < 0.010f &&
                        activeExpression != EyeExpression::Love) {
                        red = 255; green = 248; blue = 235;
                    }
                    intensity = 1.0f;
                } else {
                    // Keep the sclera slightly brighter near its center and
                    // darker at the tapered corners for a layered eye shell.
                    const float shellShade = 0.72f + 0.28f *
                        Clamp01(1.0f - std::fabs(dx) * 0.42f - std::fabs(dy) * 0.24f);
                    red = ClampColor(effect.red * shellShade);
                    green = ClampColor(effect.green * shellShade);
                    blue = ClampColor(effect.blue * shellShade);
                    intensity = 0.88f;
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
        if (effect.text.empty())
            return true;

        const float spacing = effect.textSpacing < 0.25f ? 0.25f : effect.textSpacing;
        const float textScale = effect.textScale < 0.05f ? 0.05f :
            (effect.textScale > 3.0f ? 3.0f : effect.textScale);
        const float glyphStep = spacing * textScale;
        const bool vertical =
            effect.textMotion == RunningTextMotion::ScrollUp ||
            effect.textMotion == RunningTextMotion::ScrollDown ||
            effect.textMotion == RunningTextMotion::BounceVertical;
        const float charWidth = 6.0f * glyphStep;
        const float textWidth = charWidth * static_cast<float>(effect.text.size());
        const float textHeight = 7.0f * glyphStep;
        if (textWidth <= 0.0f || textHeight <= 0.0f)
            return true;

        const float elapsed = static_cast<float>(timeMs) * speed / 180.0f;
        const float axisLength = vertical
            ? static_cast<float>(matrix.rows)
            : static_cast<float>(matrix.columns);
        const float movingExtent = vertical ? textHeight : textWidth;
        const float travel = movingExtent + axisLength;
        float textOrigin = 0.0f;

        switch (effect.textMotion) {
        case RunningTextMotion::ScrollRight:
            textOrigin = -textWidth + std::fmod(elapsed, travel);
            break;
        case RunningTextMotion::ScrollDown:
            // Move the complete horizontal message as one line. Glyphs keep
            // their left-to-right layout; no character gets its own motion.
            textOrigin = -textHeight + std::fmod(elapsed, travel);
            break;
        case RunningTextMotion::BounceHorizontal:
        case RunningTextMotion::BounceVertical: {
            const float cycle = std::fmod(elapsed, travel * 2.0f);
            textOrigin = cycle < travel
                ? axisLength - cycle
                : -movingExtent + (cycle - travel);
            break;
        }
        case RunningTextMotion::Static:
            textOrigin = (axisLength - movingExtent) * 0.5f;
            break;
        case RunningTextMotion::ScrollLeft:
        case RunningTextMotion::ScrollUp:
        default:
            textOrigin = axisLength - std::fmod(elapsed, travel);
            break;
        }

        if (!vertical) {
            // Horizontal text is centered vertically and scaled on both axes.
            // Each output LED samples the source glyph area it actually covers,
            // preventing thin strokes/colors from vanishing below 1x scale.
            const float localX = static_cast<float>(column) - textOrigin;
            const float verticalOrigin =
                (static_cast<float>(matrix.rows) - textHeight) * 0.5f;
            const float localY = static_cast<float>(row) - verticalOrigin;
            if (localX >= 0.0f && localX < textWidth &&
                localY >= 0.0f && localY < textHeight) {
                const size_t charIndex =
                    static_cast<size_t>(localX / charWidth);
                const float withinX = localX -
                    static_cast<float>(charIndex) * charWidth;
                if (charIndex < effect.text.size() && withinX < 5.0f * glyphStep) {
                    intensity = IsGlyphPixelCovered(
                        effect.text[charIndex], withinX, localY,
                        glyphStep, effect.fontName
                    ) ? 1.0f : 0.0f;
                } else {
                    intensity = 0.0f;
                }
            } else {
                intensity = 0.0f;
            }
        } else {
            // Vertical scrolling moves one complete horizontal text line.
            // Its X layout is fixed and centered; only the whole row block
            // moves on Y, so letters never travel independently.
            const float horizontalOrigin =
                (static_cast<float>(matrix.columns) - textWidth) * 0.5f;
            const float localX =
                static_cast<float>(column) - horizontalOrigin;
            const float localY =
                static_cast<float>(row) - textOrigin;
            if (localX >= 0.0f && localX < textWidth &&
                localY >= 0.0f && localY < textHeight) {
                const size_t charIndex =
                    static_cast<size_t>(localX / charWidth);
                const float withinX =
                    localX - static_cast<float>(charIndex) * charWidth;
                if (charIndex < effect.text.size() && withinX < 5.0f * glyphStep) {
                    intensity = IsGlyphPixelCovered(
                        effect.text[charIndex], withinX, localY,
                        glyphStep, effect.fontName
                    ) ? 1.0f : 0.0f;
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
            colorPhase * 360.0f +
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
        effect.type == LedEffectType::RunningText
            ? effect.motionPattern
            : localMotionFrame.pattern;
    if (effect.type != LedEffectType::StroboEyes &&
        (effect.type != LedEffectType::RunningText ||
         activePattern == LedMotionPattern::LaserWrite ||
         activePattern == LedMotionPattern::LaserWeld)) {
        // Use the frame snapshot's master phase. A per-pixel speed multiplier
        // remains available for custom matrix pixels without reselecting patterns.
        const float phase = localMotionFrame.phase;
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
        intensity *= EvaluateMotionTransition(
            localMotionFrame.transition, motionX, motionY, phase
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
            const float hue = colorPhase * 360.0f + blend * 360.0f;
            HSVtoRGB(hue, red, green, blue);
            break;
        }
        case LedColorMode::ColorCycle:
            HSVtoRGB(colorPhase * 360.0f, red, green, blue);
            break;
        case LedColorMode::ColorWave: {
            const float wave = 0.5f + 0.5f * std::sin(
                blend * TWO_PI * 2.0f - colorPhase * TWO_PI
            );
            HSVtoRGB(wave * 360.0f + colorPhase * 360.0f, red, green, blue);
            break;
        }
        case LedColorMode::ColorPulse: {
            const float pulse = 0.5f + 0.5f * std::sin(colorPhase * TWO_PI);
            red = ClampColor(static_cast<float>(red) * (1.0f - pulse) + effect.secondaryRed * pulse);
            green = ClampColor(static_cast<float>(green) * (1.0f - pulse) + effect.secondaryGreen * pulse);
            blue = ClampColor(static_cast<float>(blue) * (1.0f - pulse) + effect.secondaryBlue * pulse);
            break;
        }
        case LedColorMode::Alternating: {
            const uint32_t band = static_cast<uint32_t>(
                std::floor(blend * 8.0f + colorPhase * 8.0f)
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
