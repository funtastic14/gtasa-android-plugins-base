
#include "profile_renderer.h"

#include <cmath>
#include <cstdint>

#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/game_sa/base/Timer.h>
#include <plugin-sdk/gta_base/DrawVertices.h>
#include <plugin-sdk/renderware/RwRender.h>

namespace VLS {

namespace {

constexpr uint32_t RUNNING_LED_SEGMENTS = 12;
constexpr uint32_t RUNNING_LED_TRAIL = 4;

class RenderStateGuard {
public:
    RenderStateGuard() {
        m_hasTexture = RwRenderStateGet(
            rwRENDERSTATETEXTURERASTER,
            &m_texture
        );

        m_hasVertexAlpha = RwRenderStateGet(
            rwRENDERSTATEVERTEXALPHAENABLE,
            &m_vertexAlpha
        );

        m_hasSrcBlend = RwRenderStateGet(
            rwRENDERSTATESRCBLEND,
            &m_srcBlend
        );

        m_hasDestBlend = RwRenderStateGet(
            rwRENDERSTATEDESTBLEND,
            &m_destBlend
        );

        if (m_hasTexture) {
            RwRenderStateSet(
                rwRENDERSTATETEXTURERASTER,
                nullptr
            );
        }

        if (m_hasVertexAlpha) {
            RwRenderStateSet(
                rwRENDERSTATEVERTEXALPHAENABLE,
                StateValue(1)
            );
        }

        if (m_hasSrcBlend) {
            RwRenderStateSet(
                rwRENDERSTATESRCBLEND,
                StateValue(rwBLENDSRCALPHA)
            );
        }

        if (m_hasDestBlend) {
            RwRenderStateSet(
                rwRENDERSTATEDESTBLEND,
                StateValue(rwBLENDINVSRCALPHA)
            );
        }
    }

    ~RenderStateGuard() {
        if (m_hasTexture) {
            RwRenderStateSet(
                rwRENDERSTATETEXTURERASTER,
                m_texture
            );
        }

        if (m_hasVertexAlpha) {
            RwRenderStateSet(
                rwRENDERSTATEVERTEXALPHAENABLE,
                m_vertexAlpha
            );
        }

        if (m_hasSrcBlend) {
            RwRenderStateSet(
                rwRENDERSTATESRCBLEND,
                m_srcBlend
            );
        }

        if (m_hasDestBlend) {
            RwRenderStateSet(
                rwRENDERSTATEDESTBLEND,
                m_destBlend
            );
        }
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

static uint8_t ApplyBrightness(
    uint8_t color,
    float brightness
) {
    float result = static_cast<float>(color) * brightness;

    if (result < 0.0f) {
        result = 0.0f;
    }

    if (result > 255.0f) {
        result = 255.0f;
    }

    return static_cast<uint8_t>(result);
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

static bool RenderRunningLED(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect
) {
    if (vehicle == nullptr || !effect.enabled) {
        return false;
    }

    CMatrix* matrix = vehicle->GetMatrix();

    if (matrix == nullptr) {
        return false;
    }

    const LedTransform& transform = placement.transform;

    if (!std::isfinite(transform.x) ||
        !std::isfinite(transform.y) ||
        !std::isfinite(transform.z) ||
        !std::isfinite(transform.width) ||
        !std::isfinite(transform.height) ||
        !std::isfinite(effect.brightness) ||
        !std::isfinite(effect.speed)) {
        return false;
    }

    if (transform.width <= 0.0f ||
        transform.height <= 0.0f ||
        effect.speed <= 0.0f ||
        effect.brightness <= 0.0f) {
        return false;
    }

    float brightness = effect.brightness;

    if (brightness > 1.0f) {
        brightness = 1.0f;
    }

    const float speed = effect.speed;

    const uint32_t nowMs = CTimer::GetTimeMS();

    // Satu putaran animasi dasar berlangsung satu detik
    // pada speed 1.0.
    const double elapsed =
        static_cast<double>(nowMs) *
        static_cast<double>(speed) /
        1000.0;

    const double cycle = elapsed -
        std::floor(elapsed);

    const uint32_t head = static_cast<uint32_t>(
        cycle * RUNNING_LED_SEGMENTS
    ) % RUNNING_LED_SEGMENTS;

    const float segmentWidth =
        transform.width /
        static_cast<float>(RUNNING_LED_SEGMENTS);

    const float halfHeight = transform.height * 0.5f;

    RwIm3DVertex vertices[RUNNING_LED_SEGMENTS * 4];
    RwImVertexIndex indices[RUNNING_LED_SEGMENTS * 6];

    uint32_t indexCount = 0;

    for (uint32_t i = 0; i < RUNNING_LED_SEGMENTS; ++i) {
        const uint32_t distance =
            (head + RUNNING_LED_SEGMENTS - i) %
            RUNNING_LED_SEGMENTS;

        float intensity = 0.08f;

        if (distance < RUNNING_LED_TRAIL) {
            intensity = 1.0f -
                static_cast<float>(distance) * 0.22f;
        }

        const float segmentBrightness =
            brightness * intensity;

        const uint8_t red = ApplyBrightness(
            effect.red,
            segmentBrightness
        );

        const uint8_t green = ApplyBrightness(
            effect.green,
            segmentBrightness
        );

        const uint8_t blue = ApplyBrightness(
            effect.blue,
            segmentBrightness
        );

        const float left =
            transform.x - transform.width * 0.5f +
            static_cast<float>(i) * segmentWidth;

        const float right = left + segmentWidth * 0.88f;

        CVector localPositions[4];

        localPositions[0] = {
            left,
            transform.y - halfHeight,
            transform.z
        };

        localPositions[1] = {
            right,
            transform.y - halfHeight,
            transform.z
        };

        localPositions[2] = {
            right,
            transform.y + halfHeight,
            transform.z
        };

        localPositions[3] = {
            left,
            transform.y + halfHeight,
            transform.z
        };

        for (uint32_t vertexIndex = 0; vertexIndex < 4; ++vertexIndex) {
            const CVector worldPosition =
                matrix->TransformPoint(
                    localPositions[vertexIndex]
                );

            SetVertex(
                vertices[i * 4 + vertexIndex],
                worldPosition,
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
        rwIM3D_VERTEXXYZ |
        rwIM3D_VERTEXRGBA;

    if (RwIm3DTransform(
            vertices,
            RUNNING_LED_SEGMENTS * 4,
            nullptr,
            flags
        ) == nullptr) {
        return false;
    }

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
    if (vehicle == nullptr || !profile.enabled) {
        return 0;
    }

    uint32_t renderedCount = 0;

    for (const LedPlacement& placement : profile.placements) {
        for (const LedEffectConfig& effect : placement.effects) {
            if (effect.type == LedEffectType::RunningLED) {
                if (RenderRunningLED(
                        vehicle,
                        placement,
                        effect
                    )) {
                    ++renderedCount;
                }
            }
        }
    }

    return renderedCount;
}

} // namespace VLS
