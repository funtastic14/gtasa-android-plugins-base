
#include "profile_renderer.h"

#include <cmath>
#include <cstdint>

#include <plugin-sdk/game_sa/base/Matrix.h>
#include <plugin-sdk/gta_base/DrawVertices.h>
#include <plugin-sdk/renderware/RwRender.h>

namespace VLS {

namespace {

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

static bool RenderPlacement(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect
) {
    if (vehicle == nullptr) {
        return false;
    }

    if (!effect.enabled || effect.type != LedEffectType::RunningLED) {
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
        !std::isfinite(effect.brightness)) {
        return false;
    }

    if (transform.width <= 0.0f ||
        transform.height <= 0.0f) {
        return false;
    }

    float brightness = effect.brightness;

    if (brightness < 0.0f) {
        brightness = 0.0f;
    }

    if (brightness > 1.0f) {
        brightness = 1.0f;
    }

    if (brightness <= 0.0f) {
        return false;
    }

    const uint8_t red = ApplyBrightness(
        effect.red,
        brightness
    );

    const uint8_t green = ApplyBrightness(
        effect.green,
        brightness
    );

    const uint8_t blue = ApplyBrightness(
        effect.blue,
        brightness
    );

    const float halfWidth = transform.width * 0.5f;
    const float halfHeight = transform.height * 0.5f;

    CVector localPositions[4];

    localPositions[0] = {
        transform.x - halfWidth,
        transform.y - halfHeight,
        transform.z
    };

    localPositions[1] = {
        transform.x + halfWidth,
        transform.y - halfHeight,
        transform.z
    };

    localPositions[2] = {
        transform.x + halfWidth,
        transform.y + halfHeight,
        transform.z
    };

    localPositions[3] = {
        transform.x - halfWidth,
        transform.y + halfHeight,
        transform.z
    };

    RwIm3DVertex vertices[4];

    for (int i = 0; i < 4; ++i) {
        const CVector worldPosition =
            matrix->TransformPoint(localPositions[i]);

        SetVertex(
            vertices[i],
            worldPosition,
            red,
            green,
            blue
        );
    }

    static RwImVertexIndex indices[6] = {
        0, 1, 2,
        0, 2, 3
    };

    RenderStateGuard stateGuard;

    const RwUInt32 flags =
        rwIM3D_VERTEXXYZ |
        rwIM3D_VERTEXRGBA;

    // Vertices have already been transformed into world coordinates.
    // A null local-to-world matrix prevents applying the vehicle
    // transformation a second time.
    if (RwIm3DTransform(
            vertices,
            4,
            nullptr,
            flags
        ) == nullptr) {
        return false;
    }

    const bool rendered = RwIm3DRenderIndexedPrimitive(
        rwPRIMTYPETRILIST,
        indices,
        6
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
            if (RenderPlacement(
                    vehicle,
                    placement,
                    effect
                )) {
                ++renderedCount;
            }
        }
    }

    return renderedCount;
}

} // namespace VLS
