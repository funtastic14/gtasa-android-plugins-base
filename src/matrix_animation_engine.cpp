#include "matrix_animation_engine.h"
#include "led_motion_engine.h"
#include "profile_storage.h"
#include <mod/thirdparty/stb/stb_image.h>

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace VLS {

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

uint8_t ClampColor(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 255.0f) value = 255.0f;
    return static_cast<uint8_t>(value);
}

const EyeTextureDesign* ResolveEyeTextureDesign(
    const LedEffectConfig& effect,
    uint32_t timeMs
) {
    const size_t count = effect.eyeTextures.size();
    if (count == 0)
        return nullptr;
    size_t selected = effect.eyeTextureIndex < count
        ? effect.eyeTextureIndex : 0;
    const uint32_t interval = effect.eyeTextureIntervalMs < 500
        ? 500u : effect.eyeTextureIntervalMs;
    const uint32_t step = timeMs / interval;
    switch (effect.eyeTexturePlayback) {
    case EyeTexturePlaybackMode::Manual:
        break;
    case EyeTexturePlaybackMode::Automatic:
        selected = step % count;
        break;
    case EyeTexturePlaybackMode::Sequence:
        if (!effect.eyeTextureSequence.empty()) {
            const size_t sequenceIndex = step % effect.eyeTextureSequence.size();
            selected = effect.eyeTextureSequence[sequenceIndex] % count;
        } else {
            selected = step % count;
        }
        break;
    case EyeTexturePlaybackMode::Random: {
        uint32_t value = step ^ 0xA511E9B3u;
        value ^= value >> 16; value *= 0x7FEB352Du;
        value ^= value >> 15; value *= 0x846CA68Bu;
        value ^= value >> 16;
        selected = value % count;
        break;
    }
    }
    return &effect.eyeTextures[selected];
}

EyeMotion ResolveEyeMotionAtStep(
    const LedEffectConfig& effect,
    uint32_t step
) {
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Manual)
        return effect.eyeMotion;
    static const EyeMotion order[] = {
        EyeMotion::Idle, EyeMotion::LookLeft, EyeMotion::LookRight,
        EyeMotion::LookUp, EyeMotion::LookDown, EyeMotion::LookUpLeft,
        EyeMotion::LookUpRight, EyeMotion::LookDownLeft,
        EyeMotion::LookDownRight, EyeMotion::Scan, EyeMotion::Shake,
        EyeMotion::Wink
    };
    const uint32_t count = static_cast<uint32_t>(
        sizeof(order) / sizeof(order[0])
    );
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Automatic)
        return order[step % count];
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Sequential)
        return order[step % count];
    uint32_t value = step ^ 0x9E3779B9u;
    value ^= value >> 16; value *= 0x7FEB352Du;
    value ^= value >> 15; value *= 0x846CA68Bu;
    value ^= value >> 16;
    return order[value % count];
}

EyeMotion ResolveEyeMotion(const LedEffectConfig& effect, uint32_t timeMs) {
    const uint32_t interval = effect.eyeMotionIntervalMs < 500
        ? 500u : effect.eyeMotionIntervalMs;
    return ResolveEyeMotionAtStep(effect, timeMs / interval);
}

void GetEyeGazeForMotion(
    EyeMotion motion,
    float time,
    float& gazeX,
    float& gazeY
) {
    gazeX = std::sin(time / 900.0f) * 0.055f;
    gazeY = std::sin(time / 1250.0f) * 0.035f;
    switch (motion) {
    case EyeMotion::LookLeft: gazeX = -0.30f; gazeY = 0.0f; break;
    case EyeMotion::LookRight: gazeX = 0.30f; gazeY = 0.0f; break;
    case EyeMotion::LookUp: gazeX = 0.0f; gazeY = -0.28f; break;
    case EyeMotion::LookDown: gazeX = 0.0f; gazeY = 0.28f; break;
    case EyeMotion::LookUpLeft: gazeX = -0.24f; gazeY = -0.22f; break;
    case EyeMotion::LookUpRight: gazeX = 0.24f; gazeY = -0.22f; break;
    case EyeMotion::LookDownLeft: gazeX = -0.24f; gazeY = 0.22f; break;
    case EyeMotion::LookDownRight: gazeX = 0.24f; gazeY = 0.22f; break;
    case EyeMotion::Scan: gazeX = std::sin(time / 430.0f) * 0.31f; gazeY = 0.0f; break;
    case EyeMotion::Shake:
        gazeX = std::sin(time / 90.0f) * 0.25f;
        gazeY = std::sin(time / 130.0f) * 0.08f;
        break;
    case EyeMotion::Idle:
    case EyeMotion::Wink:
    default: break;
    }
}

void ResolveEyeGaze(
    const LedEffectConfig& effect,
    uint32_t timeMs,
    float speed,
    EyeMotion activeMotion,
    float& gazeX,
    float& gazeY
) {
    const float time = static_cast<float>(timeMs) * speed;
    GetEyeGazeForMotion(activeMotion, time, gazeX, gazeY);
    if (effect.eyeMotionPlayback == MotionPlaybackMode::Manual)
        return;

    const uint32_t interval = effect.eyeMotionIntervalMs < 500
        ? 500u : effect.eyeMotionIntervalMs;
    const uint32_t phase = timeMs % interval;
    const uint32_t transitionMs = 240u;
    if (phase >= transitionMs)
        return;

    const uint32_t step = timeMs / interval;
    if (step == 0)
        return;
    const EyeMotion previousMotion = ResolveEyeMotionAtStep(effect, step - 1u);
    float previousX = 0.0f, previousY = 0.0f;
    GetEyeGazeForMotion(previousMotion, time, previousX, previousY);
    const float p = Clamp01(static_cast<float>(phase) / transitionMs);
    const float eased = p * p * (3.0f - 2.0f * p);
    gazeX = previousX + (gazeX - previousX) * eased;
    gazeY = previousY + (gazeY - previousY) * eased;
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
    std::vector<uint8_t> pixels;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t lastAttemptMs = 0;
    bool valid = false;
};

// Shared bounded cache lets the procedural renderer detect a failed or
// evicted custom texture and keep the eye visible instead of suppressing it.
TextureCacheEntry g_eyeTextureCache[2];
uint32_t g_nextEyeTextureSlot = 0;

bool IsCustomTextureReady(const std::string& textureName) {
    if (textureName.empty())
        return false;
    for (size_t i = 0; i < 2; ++i) {
        if (g_eyeTextureCache[i].name == textureName)
            return g_eyeTextureCache[i].valid;
    }
    return false;
}

void ApplyCustomTexture(
    const LedEffectConfig& effect,
    const std::string& textureName,
    const EyeTextureDesign* design,
    const LedMatrixConfig& matrix,
    uint16_t column,
    uint16_t row,
    uint32_t timeMs,
    float speed,
    EyeMotion eyeMotion,
    float blinkAmount,
    MatrixPixelOutput& output
) {
    if (textureName.empty() || matrix.columns == 0 ||
        matrix.rows == 0 || !output.visible)
        return;

    // Two bounded decoded-image slots cap resident RGBA texture memory at
    // 8 MiB (1024x1024x4 bytes per image). Image decoding/allocation happens
    // only when a texture is first encountered, never in the sampling loop.
    TextureCacheEntry* entry = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        if (g_eyeTextureCache[i].name == textureName) {
            entry = &g_eyeTextureCache[i];
            // A missing/temporarily unreadable file is not retried per pixel.
            // Retry at most once every 3 seconds so adding a texture at runtime
            // can recover without causing repeated filesystem reads.
            if (!entry->valid && timeMs - entry->lastAttemptMs >= 3000u) {
                entry->name.clear();
                entry = nullptr;
            } else {
                break;
            }
        }
    }
    if (entry == nullptr) {
        entry = &g_eyeTextureCache[g_nextEyeTextureSlot++ % 2u];
        entry->name = textureName;
        entry->valid = false;
        entry->width = entry->height = 0;
        entry->pixels.clear();
        entry->lastAttemptMs = timeMs;

        std::string data, token;
        if (ProfileStorage::LoadTexturePng(textureName, data) &&
            !data.empty() && data.size() <= static_cast<size_t>(0x7fffffff)) {
            const stbi_uc* encoded =
                reinterpret_cast<const stbi_uc*>(data.data());
            const int encodedSize = static_cast<int>(data.size());
            int w = 0, h = 0;
            // Reject oversized images before asking stb_image to allocate
            // the decoded bitmap, keeping peak memory bounded on Android.
            if (stbi_info_from_memory(encoded, encodedSize, &w, &h, nullptr) &&
                w > 0 && w <= 1024 && h > 0 && h <= 1024) {
                stbi_uc* decoded = stbi_load_from_memory(
                    encoded, encodedSize, &w, &h, nullptr, STBI_rgb_alpha
                );
                if (decoded != nullptr) {
                    const size_t decodedBytes =
                        static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
                    entry->pixels.resize(decodedBytes);
                    std::memcpy(entry->pixels.data(), decoded, decodedBytes);
                    entry->width = static_cast<uint16_t>(w);
                    entry->height = static_cast<uint16_t>(h);
                    entry->valid = true;
                    stbi_image_free(decoded);
                }
            }
        }
        if (!entry->valid &&
            ProfileStorage::LoadTexturePpm(textureName, data)) {
            size_t pos = 0;
            if (ReadPpmToken(data, pos, token) && token == "P3" &&
                ReadPpmToken(data, pos, token)) {
                const int w = std::atoi(token.c_str());
                if (ReadPpmToken(data, pos, token)) {
                    const int h = std::atoi(token.c_str());
                    if (ReadPpmToken(data, pos, token)) {
                        const int maxValue = std::atoi(token.c_str());
                        if (w > 0 && w <= 1024 && h > 0 && h <= 1024 &&
                            maxValue > 0 && maxValue <= 255) {
                            const size_t pixelCount =
                                static_cast<size_t>(w) * static_cast<size_t>(h);
                            entry->pixels.assign(pixelCount * 4u, 0);
                            bool ok = true;
                            for (size_t i = 0; i < pixelCount * 3u; ++i) {
                                if (!ReadPpmToken(data, pos, token)) {
                                    ok = false;
                                    break;
                                }
                                const int value = std::atoi(token.c_str());
                                if (value < 0 || value > maxValue) {
                                    ok = false;
                                    break;
                                }
                                entry->pixels[(i / 3u) * 4u + (i % 3u)] =
                                    static_cast<uint8_t>((value * 255) / maxValue);
                            }
                            if (ok) {
                                for (size_t i = 0; i < pixelCount; ++i)
                                    entry->pixels[i * 4u + 3u] = 255;
                                entry->width = static_cast<uint16_t>(w);
                                entry->height = static_cast<uint16_t>(h);
                                entry->valid = true;
                            } else {
                                entry->pixels.clear();
                            }
                        }
                    }
                }
            }
        }
    }

    if (!entry->valid)
        return;
    float u = (static_cast<float>(column) + 0.5f) /
        static_cast<float>(matrix.columns);
    float v = (static_cast<float>(row) + 0.5f) /
        static_cast<float>(matrix.rows);
    if (effect.type == LedEffectType::StroboEyes) {
        // The texture canvas is split into left/right eye regions. Each half
        // receives the same gaze vector; blink compresses the source vertically.
        const bool leftEye = column < matrix.columns / 2;
        float localU = leftEye ? u * 2.0f : (u - 0.5f) * 2.0f;
        float gazeX = 0.0f;
        float gazeY = 0.0f;
        ResolveEyeGaze(effect, timeMs, speed, eyeMotion, gazeX, gazeY);
        // Use profile metadata to move the iris and pupil inside the
        // stationary sclera. If the art is flattened, the user can tune these
        // normalized regions without supplying extra animation-frame images.
        float sampleLocalU = localU;
        float sampleV = v;
        const float irisCenterX = design ? design->irisCenterX : effect.eyeTextureIrisCenterX;
        const float irisCenterY = design ? design->irisCenterY : effect.eyeTextureIrisCenterY;
        const float irisRadiusX = design ? design->irisRadiusX : effect.eyeTextureIrisRadiusX;
        const float irisRadiusY = design ? design->irisRadiusY : effect.eyeTextureIrisRadiusY;
        const float pupilCenterX = design ? design->pupilCenterX : effect.eyeTexturePupilCenterX;
        const float pupilCenterY = design ? design->pupilCenterY : effect.eyeTexturePupilCenterY;
        const float pupilRadiusX = design ? design->pupilRadiusX : effect.eyeTexturePupilRadiusX;
        const float pupilRadiusY = design ? design->pupilRadiusY : effect.eyeTexturePupilRadiusY;
        if (design != nullptr && !design->mappingConfigured) {
            // Without semantic UV metadata, deform the complete eye artwork
            // conservatively instead of guessing where iris/pupil boundaries are.
            sampleLocalU = Clamp01(localU - gazeX * 0.16f);
            sampleV = Clamp01(v - gazeY * 0.16f);
        } else {
            const float pupilDx = (localU - pupilCenterX) / pupilRadiusX;
            const float pupilDy = (v - pupilCenterY) / pupilRadiusY;
            const bool insidePupil = pupilDx * pupilDx + pupilDy * pupilDy <= 1.0f;
            const float irisDx = (localU - irisCenterX) / irisRadiusX;
            const float irisDy = (v - irisCenterY) / irisRadiusY;
            const bool insideIris = irisDx * irisDx + irisDy * irisDy <= 1.0f;

            if (insidePupil) {
                sampleLocalU -= gazeX * pupilRadiusX;
                sampleV -= gazeY * pupilRadiusY;
                sampleLocalU = std::max(
                    pupilCenterX - pupilRadiusX,
                    std::min(pupilCenterX + pupilRadiusX, sampleLocalU)
                );
                sampleV = std::max(
                    pupilCenterY - pupilRadiusY,
                    std::min(pupilCenterY + pupilRadiusY, sampleV)
                );
            } else if (insideIris) {
                sampleLocalU -= gazeX * irisRadiusX * 0.82f;
                sampleV -= gazeY * irisRadiusY * 0.82f;
                sampleLocalU = std::max(
                    irisCenterX - irisRadiusX,
                    std::min(irisCenterX + irisRadiusX, sampleLocalU)
                );
                sampleV = std::max(
                    irisCenterY - irisRadiusY,
                    std::min(irisCenterY + irisRadiusY, sampleV)
                );
            }
        }

        localU = Clamp01(sampleLocalU);
        u = leftEye ? localU * 0.5f : 0.5f + localU * 0.5f;
        v = 0.5f + (sampleV - 0.5f) *
            (1.0f - Clamp01(blinkAmount) * 0.92f);
        v = Clamp01(v);
    }

    // Bilinear filtering prevents visible stepping as the static artwork moves.
    const float fx = Clamp01(u) * static_cast<float>(entry->width - 1u);
    const float fy = Clamp01(v) * static_cast<float>(entry->height - 1u);
    const uint16_t x0 = static_cast<uint16_t>(fx);
    const uint16_t y0 = static_cast<uint16_t>(fy);
    const uint16_t x1 = static_cast<uint16_t>(x0 + (x0 + 1u < entry->width ? 1u : 0u));
    const uint16_t y1 = static_cast<uint16_t>(y0 + (y0 + 1u < entry->height ? 1u : 0u));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const size_t i00 = (static_cast<size_t>(y0) * entry->width + x0) * 4u;
    const size_t i10 = (static_cast<size_t>(y0) * entry->width + x1) * 4u;
    const size_t i01 = (static_cast<size_t>(y1) * entry->width + x0) * 4u;
    const size_t i11 = (static_cast<size_t>(y1) * entry->width + x1) * 4u;
    const auto sample = [&](size_t channel) {
        const float top = entry->pixels[i00 + channel] * (1.0f - tx) +
            entry->pixels[i10 + channel] * tx;
        const float bottom = entry->pixels[i01 + channel] * (1.0f - tx) +
            entry->pixels[i11 + channel] * tx;
        return ClampColor(top * (1.0f - ty) + bottom * ty);
    };
    output.red = sample(0);
    output.green = sample(1);
    output.blue = sample(2);
    output.alpha = static_cast<uint8_t>(
        (static_cast<uint16_t>(output.alpha) * sample(3)) / 255u
    );
    output.visible = output.visible && output.alpha > 0;
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
    // Six built-in styles plus two custom fonts fit without per-pixel cache churn.
    static FontCacheEntry cache[8];
    static uint32_t nextSlot = 0;
    FontCacheEntry* entry = nullptr;
    for (size_t i = 0; i < 8; ++i) {
        if (cache[i].name == fontName) {
            entry = &cache[i];
            break;
        }
    }
    if (entry == nullptr) {
        entry = &cache[nextSlot++ % 8u];
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
    uint8_t bits = 0;
    if (ch >= '0' && ch <= '9')
        bits = font[ch - '0'][row];
    else if (ch >= 'A' && ch <= 'Z')
        bits = font[10 + ch - 'A'][row];
    else if (ch == '-') bits = row == 3 ? 14 : 0;
    else if (ch == '.') bits = row == 6 ? 4 : 0;
    else if (ch == ':') bits = (row == 2 || row == 5) ? 4 : 0;
    else if (ch == ' ') bits = 0;
    else bits = row == 0 || row == 6 ? 31 : 17;

    if (fontName == "bold" || fontName == "wide") {
        const uint8_t expanded = static_cast<uint8_t>(bits | (bits >> 1) | ((bits << 1) & 0x1Fu));
        bits = fontName == "wide"
            ? static_cast<uint8_t>((expanded | (expanded >> 1)) & 0x1Fu)
            : expanded;
    } else if (fontName == "condensed") {
        bits = static_cast<uint8_t>(((bits >> 1) & 0x07u) << 1);
    } else if (fontName == "italic") {
        const int shift = row < 2 ? 2 : (row < 4 ? 1 : 0);
        bits = static_cast<uint8_t>((bits << shift) & 0x1Fu);
    } else if (fontName == "outline") {
        const uint8_t above = row > 0
            ? GetGlyphRow(input, static_cast<uint16_t>(row - 1), "default") : 0;
        const uint8_t below = row < 6
            ? GetGlyphRow(input, static_cast<uint16_t>(row + 1), "default") : 0;
        const uint8_t horizontalCore = static_cast<uint8_t>(bits & (bits >> 1) & ((bits << 1) & 0x1Fu));
        const uint8_t verticalCore = static_cast<uint8_t>(bits & above & below);
        bits = static_cast<uint8_t>(bits & ~(horizontalCore & verticalCore));
    }
    return static_cast<uint8_t>(bits & 0x1Fu);
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
    const float strokeBoost = glyphStep < 1.0f
        ? (1.0f - glyphStep) * 0.18f : 0.0f;
    int firstColumn = static_cast<int>(std::floor((pixelLeft - strokeBoost) / glyphStep));
    int lastColumn = static_cast<int>(std::floor((pixelRight + strokeBoost - epsilon) / glyphStep));
    int firstRow = static_cast<int>(std::floor((pixelTop - strokeBoost) / glyphStep));
    int lastRow = static_cast<int>(std::floor((pixelBottom + strokeBoost - epsilon) / glyphStep));

    if (firstColumn < 0) firstColumn = 0;
    if (firstRow < 0) firstRow = 0;
    if (lastColumn > 4) lastColumn = 4;
    if (lastRow > 6) lastRow = 6;
    if (firstColumn > 4 || firstRow > 6 || lastColumn < 0 || lastRow < 0)
        return false;

    // Area-aware nearest-cell sampling: when glyph cells are smaller than one
    // LED pixel, retain any lit source cell covered by that output pixel.
    for (int glyphRow = firstRow; glyphRow <= lastRow; ++glyphRow) {
        const float rowTop = static_cast<float>(glyphRow) * glyphStep - strokeBoost;
        const float rowBottom = static_cast<float>(glyphRow + 1) * glyphStep + strokeBoost;
        if (rowBottom <= pixelTop || rowTop >= pixelBottom)
            continue;
        const uint8_t bits = GetGlyphRow(
            character, static_cast<uint16_t>(glyphRow), fontName
        );
        for (int glyphColumn = firstColumn; glyphColumn <= lastColumn; ++glyphColumn) {
            const float columnLeft = static_cast<float>(glyphColumn) * glyphStep - strokeBoost;
            const float columnRight = static_cast<float>(glyphColumn + 1) * glyphStep + strokeBoost;
            if (columnRight <= pixelLeft || columnLeft >= pixelRight)
                continue;
            if ((bits & (1u << (4 - glyphColumn))) != 0u)
                return true;
        }
    }
    return false;
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

    // Map this physical region into a shared logical canvas. Adjacent or
    // separated placements with the same canvas dimensions now sample one
    // continuous motion field instead of restarting the shape per layer.
    const uint16_t canvasColumns = matrix.canvasEnabled
        ? matrix.canvasColumns : matrix.columns;
    const uint16_t canvasRows = matrix.canvasEnabled
        ? matrix.canvasRows : matrix.rows;
    const uint32_t canvasColumn = matrix.canvasEnabled
        ? static_cast<uint32_t>(matrix.canvasOffsetColumn) + column : column;
    const uint32_t canvasRow = matrix.canvasEnabled
        ? static_cast<uint32_t>(matrix.canvasOffsetRow) + row : row;
    const float x =
        (static_cast<float>(canvasColumn) + 0.5f) /
        static_cast<float>(canvasColumns);

    const float y =
        (static_cast<float>(canvasRow) + 0.5f) /
        static_cast<float>(canvasRows);

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
    float eyeBlinkAmount = 0.0f;
    EyeMotion activeEyeMotion = effect.eyeMotion;
    const EyeTextureDesign* activeEyeTexture =
        effect.type == LedEffectType::StroboEyes
            ? ResolveEyeTextureDesign(effect, timeMs) : nullptr;
    const std::string& activeEyeTextureName = activeEyeTexture != nullptr
        ? activeEyeTexture->textureName : effect.textureName;
    // A configured but missing/invalid image must not disable procedural
    // iris, pupil and lid rendering. Once decoded, the custom art takes over.
    const bool hasCustomEyeTexture = IsCustomTextureReady(activeEyeTextureName);

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
        // Procedural mode builds the eye from the selected shape and palettes.
        // Custom mode uses the same mask plus a bounded UV gaze/blink transform.
        if (matrix.rows >= 7 && matrix.columns >= 14) {
            const bool leftEye = column < matrix.columns / 2;
            const float eyeCenterX = leftEye
                ? static_cast<float>(matrix.columns) * 0.25f
                : static_cast<float>(matrix.columns) * 0.75f;
            const float eyeCenterY = static_cast<float>(matrix.rows - 1) * 0.5f;
            const float radiusX = static_cast<float>(matrix.columns) * 0.22f;
            const float radiusY = static_cast<float>(matrix.rows) * 0.43f;
            const float dx = (static_cast<float>(column) - eyeCenterX) / radiusX;
            const float dy = (static_cast<float>(row) - eyeCenterY) / radiusY;
            const float absX = std::fabs(dx);
            const EyeMotion activeMotion = ResolveEyeMotion(effect, timeMs);
            activeEyeMotion = activeMotion;
            float gazeX = 0.0f;
            float gazeY = 0.0f;
            ResolveEyeGaze(effect, timeMs, speed, activeMotion, gazeX, gazeY);

            bool insideEye = false;
            switch (effect.eyeShape) {
            case EyeShape::Round:
                insideEye = dx * dx + dy * dy <= 1.0f;
                break;
            case EyeShape::Sleepy:
                insideEye = dx * dx + dy * dy <= 1.0f &&
                    dy >= (-0.18f + 0.30f * absX);
                break;
            case EyeShape::Original:
            default:
                insideEye = dx * dx + dy * dy <= 1.0f &&
                    std::fabs(dy) <= (1.04f - 0.12f * absX);
                break;
            }

            const uint32_t blinkInterval = effect.eyeBlinkIntervalMs < 500
                ? 500 : effect.eyeBlinkIntervalMs;
            const float blinkPhase = std::fmod(
                static_cast<float>(timeMs) * speed,
                static_cast<float>(blinkInterval)
            );
            const auto blinkEnvelope = [](float elapsed, float duration) {
                const float p = Clamp01(elapsed / duration);
                const float triangle = 1.0f - std::fabs(2.0f * p - 1.0f);
                return triangle * triangle * (3.0f - 2.0f * triangle);
            };
            float blinkAmount = 0.0f;
            if (blinkPhase < 150.0f)
                blinkAmount = blinkEnvelope(blinkPhase, 150.0f);
            const float secondBlinkStart = static_cast<float>(blinkInterval) * 0.72f;
            if (blinkPhase >= secondBlinkStart &&
                blinkPhase < secondBlinkStart + 130.0f) {
                blinkAmount = std::max(
                    blinkAmount,
                    blinkEnvelope(blinkPhase - secondBlinkStart, 130.0f)
                );
            }

            if (activeMotion == EyeMotion::Wink) {
                // Wink follows the same speed-scaled animation clock as gaze
                // and regular blinking, while keeping a stable 4.8s cycle.
                const float winkPhase = std::fmod(
                    static_cast<float>(timeMs) * speed, 4800.0f
                );
                float winkAmount = 0.0f;
                if (leftEye && winkPhase < 420.0f) {
                    winkAmount = blinkEnvelope(winkPhase, 420.0f);
                } else if (!leftEye &&
                           winkPhase >= 2400.0f && winkPhase < 2820.0f) {
                    winkAmount = blinkEnvelope(
                        winkPhase - 2400.0f, 420.0f
                    );
                }
                // Fade each wink smoothly instead of snapping the lid shut.
                blinkAmount = std::max(blinkAmount, winkAmount);
            }
            eyeBlinkAmount = blinkAmount;

            if (insideEye) {
                if (blinkAmount > 0.01f && !hasCustomEyeTexture) {
                    // Procedural eyelid closes to a narrow slit. Custom art
                    // instead uses vertical UV compression in ApplyCustomTexture.
                    intensity = std::fabs(dy) < (0.025f + 0.08f * (1.0f - blinkAmount))
                        ? 1.0f : 0.0f;
                } else if (!hasCustomEyeTexture) {
                    const float pupilX = dx - gazeX;
                    const float pupilY = dy - gazeY;
                    const float pupilDistance = pupilX * pupilX + pupilY * pupilY;
                    if (pupilDistance < 0.115f * 0.115f) {
                        preserveEyeDetailColor = true;
                        red = effect.eyePupilRed;
                        green = effect.eyePupilGreen;
                        blue = effect.eyePupilBlue;
                    } else if (pupilDistance < 0.34f * 0.34f) {
                        preserveEyeDetailColor = true;
                        red = effect.eyeIrisRed;
                        green = effect.eyeIrisGreen;
                        blue = effect.eyeIrisBlue;
                        // A restrained radial ring adds depth without restoring
                        // the retired Sharingan/Rinnegan motif presets.
                        const float radial = std::sqrt(pupilDistance);
                        if (radial > 0.285f) {
                            red = static_cast<uint8_t>((static_cast<unsigned int>(red) * 3u) / 4u);
                            green = static_cast<uint8_t>((static_cast<unsigned int>(green) * 3u) / 4u);
                            blue = static_cast<uint8_t>((static_cast<unsigned int>(blue) * 3u) / 4u);
                        }
                    } else {
                        red = effect.red;
                        green = effect.green;
                        blue = effect.blue;
                    }
                    intensity = 1.0f;
                } else {
                    // Custom artwork keeps its authored colors; its UV transform
                    // is applied after the procedural mask and blink state are resolved.
                    intensity = 1.0f;
                }
            } else {
                intensity = 0.0f;
            }
        } else {
            const uint32_t blinkInterval = effect.eyeBlinkIntervalMs < 500
                ? 500 : effect.eyeBlinkIntervalMs;
            const float blinkPhase = std::fmod(
                static_cast<float>(timeMs) * speed,
                static_cast<float>(blinkInterval)
            );
            intensity = blinkPhase < 140.0f ? 0.0f : 1.0f;
        }
        if (!preserveEyeDetailColor) {
            red = effect.red;
            green = effect.green;
            blue = effect.blue;
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
            matrix.canvasEnabled ? static_cast<uint16_t>(canvasColumn) : column,
            matrix.canvasEnabled ? static_cast<uint16_t>(canvasRow) : row,
            canvasColumns, canvasRows
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
    if (effect.transparentMatrixBackground && intensity <= 0.055f)
        output.alpha = 0;
    output.visible = output.intensity > 0.001f && output.alpha > 0;
    ApplyCustomTexture(effect, activeEyeTextureName, activeEyeTexture,
                       matrix, column, row, timeMs, speed,
                       activeEyeMotion, eyeBlinkAmount, output);

    return true;
}

} // namespace VLS
