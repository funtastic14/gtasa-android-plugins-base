
#include "profile_renderer.h"

#include <cmath>
#include <cstdint>

#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/gta_base/DrawVertices.h>
#include <plugin-sdk/renderware/RwRender.h>

namespace VLS {

namespace {

constexpr uint32_t SEGMENTS = 12;

class RenderStateGuard {
public:
    RenderStateGuard() {
        m_hasTexture = RwRenderStateGet(
            rwRENDERSTATETEXTURERASTER, &m_texture
        );
        m_hasVertexAlpha = RwRenderStateGet(
            rwRENDERSTATEVERTEXALPHAENABLE, &m_vertexAlpha
        );
        m_hasSrcBlend = RwRenderStateGet(
            rwRENDERSTATESRCBLEND, &m_srcBlend
        );
        m_hasDestBlend = RwRenderStateGet(
            rwRENDERSTATEDESTBLEND, &m_destBlend
        );

        if (m_hasTexture)
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);

        if (m_hasVertexAlpha)
            RwRenderStateSet(
                rwRENDERSTATEVERTEXALPHAENABLE,
                StateValue(1)
            );

        if (m_hasSrcBlend)
            RwRenderStateSet(
                rwRENDERSTATESRCBLEND,
                StateValue(rwBLENDSRCALPHA)
            );

        if (m_hasDestBlend)
            RwRenderStateSet(
                rwRENDERSTATEDESTBLEND,
                StateValue(rwBLENDINVSRCALPHA)
            );
    }

    ~RenderStateGuard() {
        if (m_hasTexture)
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, m_texture);

        if (m_hasVertexAlpha)
            RwRenderStateSet(
                rwRENDERSTATEVERTEXALPHAENABLE, m_vertexAlpha
            );

        if (m_hasSrcBlend)
            RwRenderStateSet(rwRENDERSTATESRCBLEND, m_srcBlend);

        if (m_hasDestBlend)
            RwRenderStateSet(rwRENDERSTATEDESTBLEND, m_destBlend);
    }

    RenderStateGuard(const RenderStateGuard&) = delete;
    RenderStateGuard& operator=(const RenderStateGuard&) = delete;

private:
    static void* StateValue(uintptr_t value) {
        return reinterpret_cast<void*>(value);
    }

    void* m_texture = nullptr;
    void* m_vertexAlpha = nullptr;
    void* m_srcBlend = nullptr;
    void* m_destBlend = nullptr;

    bool m_hasTexture = false;
    bool m_hasVertexAlpha = false;
    bool m_hasSrcBlend = false;
    bool m_hasDestBlend = false;
};

static uint8_t ClampColor(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 255.0f) value = 255.0f;
    return static_cast<uint8_t>(value);
}

static void HSVtoRGB(
    float hue,
    uint8_t& red,
    uint8_t& green,
    uint8_t& blue
) {
    while (hue < 0.0f) hue += 360.0f;
    while (hue >= 360.0f) hue -= 360.0f;

    const float c = 1.0f;
    const float x = c * (
        1.0f - std::fabs(
            std::fmod(hue / 60.0f, 2.0f) - 1.0f
        )
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

static void SetVertex(
    RwIm3DVertex& vertex,
    const CVector& position,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    vertex.x = position.x;
    vertex.y = position.y;
    vertex.z = position.z;

    vertex.normal.x = 0.0f;
    vertex.normal.y = 0.0f;
    vertex.normal.z = 1.0f;

    vertex.r = red;
    vertex.g = green;
    vertex.b = blue;
    vertex.a = 255;

    vertex.u = 0.0f;
    vertex.v = 0.0f;
}

static bool RenderEffect(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect
) {
    if (vehicle == nullptr || !effect.enabled)
        return false;

    CMatrix* matrix = vehicle->GetMatrix();
    if (matrix == nullptr)
        return false;

    const LedTransform& t = placement.transform;

    if (!std::isfinite(t.x) ||
        !std::isfinite(t.y) ||
        !std::isfinite(t.z) ||
        !std::isfinite(t.width) ||
        !std::isfinite(t.height) ||
        !std::isfinite(effect.brightness) ||
        !std::isfinite(effect.speed))
        return false;

    if (t.width <= 0.0f ||
        t.height <= 0.0f ||
        effect.brightness <= 0.0f ||
        effect.speed <= 0.0f)
        return false;

    float brightness = effect.brightness;
    if (brightness > 1.0f)
        brightness = 1.0f;

    const uint32_t now = CTimer::GetTimeMS();
    const double time = static_cast<double>(now) *
                        static_cast<double>(effect.speed);

    const double cycle = std::fmod(time / 1000.0, 1.0);

    const uint32_t head = static_cast<uint32_t>(
        cycle * SEGMENTS
    ) % SEGMENTS;

    const float segmentWidth = t.width / SEGMENTS;
    const float halfHeight = t.height * 0.5f;

    RwIm3DVertex vertices[SEGMENTS * 4];
    RwImVertexIndex indices[SEGMENTS * 6];

    uint32_t indexCount = 0;

    bool strobeOn = std::fmod(time / 250.0, 1.0) < 0.5;

    for (uint32_t i = 0; i < SEGMENTS; ++i) {
        float intensity = 1.0f;
        uint8_t red = effect.red;
        uint8_t green = effect.green;
        uint8_t blue = effect.blue;

        switch (effect.type) {
        case LedEffectType::RunningLED: {
            const uint32_t distance =
                (head + SEGMENTS - i) % SEGMENTS;

            intensity = 0.08f;

            if (distance < 4)
                intensity = 1.0f - distance * 0.22f;

            break;
        }

        case LedEffectType::StroboWave: {
            const uint32_t distance =
                (head + SEGMENTS - i) % SEGMENTS;

            intensity = (distance < 3 && strobeOn) ? 1.0f : 0.0f;
            break;
        }

        case LedEffectType::StroboEyes: {
            const bool firstHalf = i < SEGMENTS / 2;
            const bool alternate = std::fmod(time / 180.0, 1.0) < 0.5;

            intensity = (firstHalf == alternate) ? 1.0f : 0.0f;
            break;
        }

        case LedEffectType::RGB: {
            HSVtoRGB(
                static_cast<float>(
                    std::fmod(time * 0.06 + i * 30.0, 360.0)
                ),
                red,
                green,
                blue
            );
            break;
        }

        default:
            return false;
        }

        red = ClampColor(red * brightness * intensity);
        green = ClampColor(green * brightness * intensity);
        blue = ClampColor(blue * brightness * intensity);

        const float left =
            t.x - t.width * 0.5f + i * segmentWidth;

        const float right = left + segmentWidth * 0.88f;

        CVector local[4];

        local[0] = {left, t.y - halfHeight, t.z};
        local[1] = {right, t.y - halfHeight, t.z};
        local[2] = {right, t.y + halfHeight, t.z};
        local[3] = {left, t.y + halfHeight, t.z};

        for (uint32_t v = 0; v < 4; ++v) {
            const CVector world = matrix->TransformPoint(local[v]);

            SetVertex(
                vertices[i * 4 + v],
                world,
                red,
                green,
                blue
            );
        }

        const RwImVertexIndex base =
            static_cast<RwImVertexIndex>(i * 4);

        indices[indexCount++] = base;
        indices[indexCount++] = base + 1;
        indices[indexCount++] = base + 2;

        indices[indexCount++] = base;
        indices[indexCount++] = base + 2;
        indices[indexCount++] = base + 3;
    }

    RenderStateGuard stateGuard;

    const RwUInt32 flags =
        rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA;

    if (RwIm3DTransform(
            vertices,
            SEGMENTS * 4,
            nullptr,
            flags
        ) == nullptr)
        return false;

    const bool rendered = RwIm3DRenderIndexedPrimitive(
        rwPRIMTYPETRILIST,
        indices,
        indexCount
    );

    RwIm3DEnd();

    return rendered;
}

} // namespace

uint32_t ProfileRenderer::RenderVehicleProfile(
    CVehicle* vehicle,
    const VehicleProfile& profile
) {
    if (vehicle == nullptr || !profile.enabled)
        return 0;

    uint32_t rendered = 0;

    for (const LedPlacement& placement : profile.placements) {
        for (const LedEffectConfig& effect : placement.effects) {
            if (RenderEffect(vehicle, placement, effect))
                ++rendered;
        }
    }

    return rendered;
}

} // namespace VLS
