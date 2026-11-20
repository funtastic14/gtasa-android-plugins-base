#include "profile_renderer.h"
#include "matrix_animation_engine.h"

#include <cmath>
#include <cstdint>
#include <vector>

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
        Save(rwRENDERSTATETEXTURERASTER, m_texture, m_hasTexture);
        Save(rwRENDERSTATEVERTEXALPHAENABLE, m_vertexAlpha, m_hasVertexAlpha);
        Save(rwRENDERSTATESRCBLEND, m_srcBlend, m_hasSrcBlend);
        Save(rwRENDERSTATEDESTBLEND, m_destBlend, m_hasDestBlend);
        Save(rwRENDERSTATEZTESTENABLE, m_zTest, m_hasZTest);
        Save(rwRENDERSTATEZWRITEENABLE, m_zWrite, m_hasZWrite);
        Save(rwRENDERSTATECULLMODE, m_cullMode, m_hasCullMode);

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, Value(1));
        RwRenderStateSet(rwRENDERSTATESRCBLEND, Value(rwBLENDSRCALPHA));
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, Value(rwBLENDINVSRCALPHA));

        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, Value(0));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, Value(0));
        RwRenderStateSet(rwRENDERSTATECULLMODE, Value(rwCULLMODECULLNONE));
    }

    ~RenderStateGuard() {
        Restore(rwRENDERSTATETEXTURERASTER, m_texture, m_hasTexture);
        Restore(rwRENDERSTATEVERTEXALPHAENABLE, m_vertexAlpha, m_hasVertexAlpha);
        Restore(rwRENDERSTATESRCBLEND, m_srcBlend, m_hasSrcBlend);
        Restore(rwRENDERSTATEDESTBLEND, m_destBlend, m_hasDestBlend);
        Restore(rwRENDERSTATEZTESTENABLE, m_zTest, m_hasZTest);
        Restore(rwRENDERSTATEZWRITEENABLE, m_zWrite, m_hasZWrite);
        Restore(rwRENDERSTATECULLMODE, m_cullMode, m_hasCullMode);
    }

    RenderStateGuard(const RenderStateGuard&) = delete;
    RenderStateGuard& operator=(const RenderStateGuard&) = delete;

private:
    static void* Value(uintptr_t value) {
        return reinterpret_cast<void*>(value);
    }

    static void Save(RwRenderState state, void*& value, bool& valid) {
        valid = RwRenderStateGet(state, &value);
    }

    static void Restore(RwRenderState state, void* value, bool valid) {
        if (valid)
            RwRenderStateSet(state, value);
    }

    void* m_texture = nullptr;
    void* m_vertexAlpha = nullptr;
    void* m_srcBlend = nullptr;
    void* m_destBlend = nullptr;
    void* m_zTest = nullptr;
    void* m_zWrite = nullptr;
    void* m_cullMode = nullptr;

    bool m_hasTexture = false;
    bool m_hasVertexAlpha = false;
    bool m_hasSrcBlend = false;
    bool m_hasDestBlend = false;
    bool m_hasZTest = false;
    bool m_hasZWrite = false;
    bool m_hasCullMode = false;
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
    vertex.normal.y = -1.0f;
    vertex.normal.z = 0.0f;

    vertex.r = red;
    vertex.g = green;
    vertex.b = blue;
    vertex.a = 255;

    vertex.u = 0.0f;
    vertex.v = 0.0f;
}

static bool RenderMatrixEffect(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect
) {
    if (vehicle == nullptr || !effect.matrix.enabled)
        return false;

    CMatrix* matrix = vehicle->GetMatrix();
    if (matrix == nullptr)
        return false;

    const uint32_t columns = effect.matrix.columns;
    const uint32_t rows = effect.matrix.rows;

    if (columns == 0 || rows == 0)
        return false;

    const uint32_t pixelCount = columns * rows;

    if (pixelCount > 4096)
        return false;

    const LedTransform& t = placement.transform;

    if (!std::isfinite(t.x) ||
        !std::isfinite(t.y) ||
        !std::isfinite(t.z) ||
        !std::isfinite(t.width) ||
        !std::isfinite(t.height) ||
        t.width <= 0.0f ||
        t.height <= 0.0f)
        return false;

    const float cellWidth = t.width / static_cast<float>(columns);
    const float cellHeight = t.height / static_cast<float>(rows);

    const float gapX = cellWidth * 0.12f;
    const float gapZ = cellHeight * 0.12f;

    const float pixelWidth = cellWidth - gapX;
    const float pixelHeight = cellHeight - gapZ;

    if (pixelWidth <= 0.0f || pixelHeight <= 0.0f)
        return false;

    std::vector<RwIm3DVertex> vertices;
    std::vector<RwImVertexIndex> indices;

    vertices.resize(pixelCount * 4);
    indices.resize(pixelCount * 6);

    const uint32_t timeMs = CTimer::GetTimeMS();

    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            MatrixPixelOutput output;

            if (!MatrixAnimationEngine::EvaluatePixel(
                    effect,
                    static_cast<uint16_t>(column),
                    static_cast<uint16_t>(row),
                    timeMs,
                    output
                ))
                return false;

            const uint32_t pixelIndex = row * columns + column;
            const uint32_t vertexBase = pixelIndex * 4;
            const uint32_t indexBase = pixelIndex * 6;

            const float left =
                t.x - t.width * 0.5f +
                static_cast<float>(column) * cellWidth +
                gapX * 0.5f;

            const float right = left + pixelWidth;

            const float top =
                t.z + t.height * 0.5f -
                static_cast<float>(row) * cellHeight -
                gapZ * 0.5f;

            const float bottom = top - pixelHeight;

            const uint8_t red =
                ClampColor(output.red * output.intensity);

            const uint8_t green =
                ClampColor(output.green * output.intensity);

            const uint8_t blue =
                ClampColor(output.blue * output.intensity);

            CVector local[4];

            local[0] = {left,  t.y, bottom};
            local[1] = {right, t.y, bottom};
            local[2] = {right, t.y, top};
            local[3] = {left,  t.y, top};

            for (uint32_t v = 0; v < 4; ++v) {
                CVector world = matrix->TransformPoint(local[v]);

                SetVertex(
                    vertices[vertexBase + v],
                    world,
                    red,
                    green,
                    blue
                );
            }

            const RwImVertexIndex base =
                static_cast<RwImVertexIndex>(vertexBase);

            indices[indexBase]     = base;
            indices[indexBase + 1] = base + 1;
            indices[indexBase + 2] = base + 2;
            indices[indexBase + 3] = base;
            indices[indexBase + 4] = base + 2;
            indices[indexBase + 5] = base + 3;
        }
    }

    RenderStateGuard stateGuard;

    const RwUInt32 flags =
        rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA;

    if (RwIm3DTransform(
            vertices.data(),
            static_cast<RwUInt32>(vertices.size()),
            nullptr,
            flags
        ) == nullptr)
        return false;

    const bool rendered = RwIm3DRenderIndexedPrimitive(
        rwPRIMTYPETRILIST,
        indices.data(),
        static_cast<RwUInt32>(indices.size())
    );

    RwIm3DEnd();

    return rendered;
}

static bool RenderEffect(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect
) {
    if (vehicle == nullptr || !effect.enabled)
        return false;

    if (effect.matrix.enabled)
        return RenderMatrixEffect(vehicle, placement, effect);

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

    const double time =
        static_cast<double>(CTimer::GetTimeMS()) *
        static_cast<double>(effect.speed);

    const double cycle = std::fmod(time / 1000.0, 1.0);

    const uint32_t head =
        static_cast<uint32_t>(cycle * SEGMENTS) % SEGMENTS;

    const float segmentWidth = t.width / SEGMENTS;
    const float halfHeight = t.height * 0.5f;

    RwIm3DVertex vertices[SEGMENTS * 4];
    RwImVertexIndex indices[SEGMENTS * 6];

    uint32_t indexCount = 0;

    const bool strobeOn =
        std::fmod(time / 250.0, 1.0) < 0.5;

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

            intensity =
                (distance < 3 && strobeOn) ? 1.0f : 0.0f;

            break;
        }

        case LedEffectType::StroboEyes: {
            const bool firstHalf = i < SEGMENTS / 2;
            const bool alternate =
                std::fmod(time / 180.0, 1.0) < 0.5;

            intensity =
                (firstHalf == alternate) ? 1.0f : 0.0f;

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

        const float right =
            left + segmentWidth * 0.88f;

        CVector local[4];

        local[0] = {left,  t.y, t.z - halfHeight};
        local[1] = {right, t.y, t.z - halfHeight};
        local[2] = {right, t.y, t.z + halfHeight};
        local[3] = {left,  t.y, t.z + halfHeight};

        for (uint32_t v = 0; v < 4; ++v) {
            CVector world = matrix->TransformPoint(local[v]);

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
