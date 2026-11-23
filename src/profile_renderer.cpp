
#include "profile_renderer.h"
#include "matrix_animation_engine.h"
#include "led_motion_engine.h"
#include "vehicle_geometry_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

        // Depth-test prevents LEDs from showing through vehicle body panels.
        // Keep depth writes disabled for transparent LED rendering.
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, Value(1));
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

static CVector TransformPlacementLocalPoint(const CVector& point, const LedTransform& t) {
    const float k = 0.01745329251994329577f;
    const float rx=(t.rotationX + t.facingPitch)*k;
    const float ry=(t.rotationY + t.facingYaw)*k;
    const float rz=t.rotationZ*k;
    float x=point.x-t.x, y=point.y-t.y, z=point.z-t.z;

    // Cylindrical/parabolic bend across the LED width. Zero curvature
    // preserves the original flat placement exactly.
    const float halfWidth = 0.5f * t.width * t.stretchX;
    if (halfWidth > 0.0001f && std::isfinite(t.curvature)) {
        const float u = std::max(-1.0f, std::min(1.0f, x / halfWidth));
        y += t.curvature * (1.0f - u * u);
    }
    float c=std::cos(rx), s=std::sin(rx), q=y*c-z*s; z=y*s+z*c; y=q;
    c=std::cos(ry); s=std::sin(ry); q=x*c+z*s; z=-x*s+z*c; x=q;
    c=std::cos(rz); s=std::sin(rz); q=x*c-y*s; y=x*s+y*c; x=q;
    return CVector(t.x+x,t.y+y,t.z+z);
}

static CVector TransformPlacementPoint(const CVector& point, const LedTransform& t, CMatrix* matrix) {
    CVector local = TransformPlacementLocalPoint(point, t);
    return matrix->TransformPoint(local);
}

static CVector ClosestPointOnTriangle(
    const CVector& p, const CVector& a, const CVector& b, const CVector& c
) {
    const CVector ab = b - a;
    const CVector ac = c - a;
    const CVector ap = p - a;
    const float d1 = DotProduct(ab, ap);
    const float d2 = DotProduct(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const CVector bp = p - b;
    const float d3 = DotProduct(ab, bp);
    const float d4 = DotProduct(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + ab * v;
    }

    const CVector cp = p - c;
    const float d5 = DotProduct(ab, cp);
    const float d6 = DotProduct(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const CVector edge = c - b;
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + edge * w;
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
}

static const SurfacePatch* FindWindshieldPatch(
    const CVector& center,
    const std::vector<AtomicMesh>& meshes,
    const std::vector<SurfacePatch>& patches,
    const AtomicMesh*& selectedMesh
) {
    const SurfacePatch* bestPatch = nullptr;
    selectedMesh = nullptr;
    float bestDistance = std::numeric_limits<float>::max();

    for (const SurfacePatch& patch : patches) {
        const bool frontCandidate =
            patch.role == SurfaceRoleCandidate::FrontGlass ||
            (patch.role == SurfaceRoleCandidate::GlassUnresolved &&
             patch.centroid.y > 0.25f &&
             std::fabs(patch.normal.y) >= std::fabs(patch.normal.x) &&
             std::fabs(patch.normal.y) >= std::fabs(patch.normal.z));
        if (!frontCandidate || patch.triangleIndices.empty())
            continue;

        const auto meshIt = std::find_if(
            meshes.begin(), meshes.end(),
            [&patch](const AtomicMesh& mesh) {
                return mesh.atomicIndex == patch.atomicIndex;
            }
        );
        if (meshIt == meshes.end() ||
            meshIt->vehicleSpaceVertices.empty())
            continue;

        float nearest = std::numeric_limits<float>::max();
        for (uint32_t triIndex : patch.triangleIndices) {
            if (triIndex >= meshIt->triangles.size()) continue;
            const GeometryTriangle& tri = meshIt->triangles[triIndex];
            if (tri.a >= meshIt->vehicleSpaceVertices.size() ||
                tri.b >= meshIt->vehicleSpaceVertices.size() ||
                tri.c >= meshIt->vehicleSpaceVertices.size()) continue;

            const GeometryVertex& av = meshIt->vehicleSpaceVertices[tri.a];
            const GeometryVertex& bv = meshIt->vehicleSpaceVertices[tri.b];
            const GeometryVertex& cv = meshIt->vehicleSpaceVertices[tri.c];
            const CVector a(av.x,av.y,av.z), b(bv.x,bv.y,bv.z), c(cv.x,cv.y,cv.z);
            const CVector q = ClosestPointOnTriangle(center,a,b,c);
            const CVector delta = center-q;
            const float distance = DotProduct(delta,delta);
            if (distance < nearest) nearest = distance;
        }

        if (nearest < bestDistance) {
            bestDistance = nearest;
            bestPatch = &patch;
            selectedMesh = &(*meshIt);
        }
    }

    // Choose the closest front-glass component even when the editor's initial
    // XYZ position is still at the vehicle origin. The LED is then projected
    // onto that real surface instead of being rejected or rendered floating.
    return bestPatch;
}

static bool ProjectToSurfacePatch(
    const CVector& point,
    const SurfacePatch& patch,
    const AtomicMesh& mesh,
    CVector& projected
) {
    float bestDistance = std::numeric_limits<float>::max();
    bool found = false;

    for (uint32_t triIndex : patch.triangleIndices) {
        if (triIndex >= mesh.triangles.size()) continue;
        const GeometryTriangle& tri = mesh.triangles[triIndex];
        if (tri.a >= mesh.vehicleSpaceVertices.size() ||
            tri.b >= mesh.vehicleSpaceVertices.size() ||
            tri.c >= mesh.vehicleSpaceVertices.size()) continue;

        const GeometryVertex& av = mesh.vehicleSpaceVertices[tri.a];
        const GeometryVertex& bv = mesh.vehicleSpaceVertices[tri.b];
        const GeometryVertex& cv = mesh.vehicleSpaceVertices[tri.c];
        const CVector a(av.x,av.y,av.z), b(bv.x,bv.y,bv.z), c(cv.x,cv.y,cv.z);
        const CVector q = ClosestPointOnTriangle(point,a,b,c);
        const CVector delta = point-q;
        const float distance = DotProduct(delta,delta);
        if (distance < bestDistance) {
            bestDistance = distance;
            projected = q;
            found = true;
        }
    }

    if (!found)
        return false;

    CVector normal(patch.normal.x,patch.normal.y,patch.normal.z);
    const float length = std::sqrt(DotProduct(normal,normal));
    if (length > 1e-5f) {
        normal *= (0.012f / length);
        projected += normal;
    }
    return true;
}

static void SetVertex(
    RwIm3DVertex& vertex,
    const CVector& position,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha = 255
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
    vertex.a = alpha;

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
        !std::isfinite(t.stretchX) ||
        !std::isfinite(t.stretchY) ||
        t.width <= 0.0f ||
        t.height <= 0.0f ||
        t.stretchX <= 0.0f ||
        t.stretchY <= 0.0f)
        return false;

    const float effectiveWidth = t.width * t.stretchX;
    const float effectiveHeight = t.height * t.stretchY;
    const float cellWidth = effectiveWidth / static_cast<float>(columns);
    const float cellHeight = effectiveHeight / static_cast<float>(rows);

    const float gapX = cellWidth * 0.12f;
    const float gapZ = cellHeight * 0.12f;

    const float pixelWidth = cellWidth - gapX;
    const float pixelHeight = cellHeight - gapZ;

    if (pixelWidth <= 0.0f || pixelHeight <= 0.0f)
        return false;

    // Reuse capacity across frames: matrix effects can run on several vehicles,
    // so allocating two fresh vectors for every placement every frame is costly on mobile.
    static std::vector<RwIm3DVertex> vertices;
    static std::vector<RwImVertexIndex> indices;
    vertices.clear();
    indices.clear();
    if (vertices.capacity() < pixelCount * 4)
        vertices.reserve(pixelCount * 4);
    if (indices.capacity() < pixelCount * 6)
        indices.reserve(pixelCount * 6);

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

            // Invisible pixels do not need vertices or triangles at all.
            if (!output.visible || output.intensity <= 0.001f)
                continue;

            const uint32_t vertexBase = static_cast<uint32_t>(vertices.size());

            const float left =
                t.x - effectiveWidth * 0.5f +
                static_cast<float>(column) * cellWidth +
                gapX * 0.5f;
            const float right = left + pixelWidth;
            const float top =
                t.z + effectiveHeight * 0.5f -
                static_cast<float>(row) * cellHeight -
                gapZ * 0.5f;
            const float bottom = top - pixelHeight;

            const uint8_t red = ClampColor(output.red * output.intensity);
            const uint8_t green = ClampColor(output.green * output.intensity);
            const uint8_t blue = ClampColor(output.blue * output.intensity);

            CVector local[4];
            local[0] = {left,  t.y, bottom};
            local[1] = {right, t.y, bottom};
            local[2] = {right, t.y, top};
            local[3] = {left,  t.y, top};

            for (uint32_t v = 0; v < 4; ++v) {
                CVector world = TransformPlacementPoint(local[v], t, matrix);
                RwIm3DVertex vertex;
                SetVertex(vertex, world, red, green, blue, output.alpha);
                vertices.push_back(vertex);
            }

            const RwImVertexIndex base =
                static_cast<RwImVertexIndex>(vertexBase);
            indices.push_back(base);
            indices.push_back(static_cast<RwImVertexIndex>(base + 1));
            indices.push_back(static_cast<RwImVertexIndex>(base + 2));
            indices.push_back(base);
            indices.push_back(static_cast<RwImVertexIndex>(base + 2));
            indices.push_back(static_cast<RwImVertexIndex>(base + 3));
        }
    }

    if (vertices.empty() || indices.empty())
        return false;

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
        !std::isfinite(t.stretchX) ||
        !std::isfinite(t.stretchY) ||
        !std::isfinite(t.rotationX) ||
        !std::isfinite(t.rotationY) ||
        !std::isfinite(t.rotationZ) ||
        !std::isfinite(t.curvature) ||
        !std::isfinite(t.facingYaw) ||
        !std::isfinite(t.facingPitch) ||
        !std::isfinite(effect.brightness) ||
        !std::isfinite(effect.opacity) ||
        !std::isfinite(effect.speed))
        return false;

    if (t.width <= 0.0f ||
        t.height <= 0.0f ||
        t.stretchX <= 0.0f ||
        t.stretchY <= 0.0f ||
        effect.brightness <= 0.0f ||
        effect.opacity <= 0.0f ||
        effect.speed <= 0.0f)
        return false;

    float brightness = effect.brightness;
    if (brightness > 1.0f)
        brightness = 1.0f;

    const double time =
        static_cast<double>(CTimer::GetTimeMS()) *
        static_cast<double>(effect.speed);

    const bool isWindshieldZone =
        placement.zone == LedZone::Windshield ||
        placement.zone == LedZone::FrontUpWindshield;
    const AtomicMesh* surfaceMesh = nullptr;
    const SurfacePatch* surfacePatch = nullptr;
    bool projectToWindshield = false;
    if (isWindshieldZone) {
        const CVector center(t.x,t.y,t.z);
        surfacePatch = FindWindshieldPatch(
            center,
            VehicleGeometryReader::GetMeshes(),
            VehicleGeometryReader::GetSurfacePatches(),
            surfaceMesh
        );
        // Surface recognition is an enhancement, not a hard render
        // dependency. Keep the established vehicle-local renderer alive
        // when a model has no readable CPU glass geometry or no valid patch.
        projectToWindshield = surfacePatch != nullptr && surfaceMesh != nullptr;

        // Projecting every rotated corner back onto the glass cancels the
        // intended tilt. When the user rotates the placement, use the full
        // vehicle-local transform so the orientation remains visible.
        const bool hasPlacementRotation =
            std::fabs(t.rotationX) > 0.001f ||
            std::fabs(t.rotationY) > 0.001f ||
            std::fabs(t.rotationZ) > 0.001f ||
            std::fabs(t.facingYaw) > 0.001f ||
            std::fabs(t.facingPitch) > 0.001f;
        if (hasPlacementRotation)
            projectToWindshield = false;
    }

    const float effectiveWidth = t.width * t.stretchX;
    const float effectiveHeight = t.height * t.stretchY;
    const float segmentWidth = effectiveWidth / SEGMENTS;
    const float halfHeight = effectiveHeight * 0.5f;

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

        // The selected motion pattern is the sole spatial movement controller.
        // Effect type supplies its visual character (flash/color/shape), not a
        // second hidden left-to-right sweep that would fight Zoom/Center/Flow.
        switch (effect.type) {
        case LedEffectType::RunningLED:
            intensity = 1.0f;
            break;

        case LedEffectType::StroboWave:
            intensity = strobeOn ? 1.0f : 0.0f;
            break;

        case LedEffectType::StroboEyes:
            // The strip renderer has no 2D eye geometry; let the selected
            // motion pattern control its spatial envelope consistently.
            intensity = 1.0f;
            break;

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

        // Only general-purpose effects sample the shared motion field.
        // Strobo Eyes and Running Text retain their dedicated movement engines.
        if (effect.type == LedEffectType::RunningLED ||
            effect.type == LedEffectType::RGB ||
            effect.type == LedEffectType::StroboWave) {
            const float phase01 = static_cast<float>(std::fmod(time / 1800.0, 1.0));
            const float nx = (static_cast<float>(i) + 0.5f) / static_cast<float>(SEGMENTS);
            intensity *= EvaluateMotionIntensity(
                ResolveMotionPattern(effect, static_cast<uint32_t>(time)), nx, 0.5f, phase01,
                static_cast<uint16_t>(i), 0,
                static_cast<uint16_t>(SEGMENTS), 1
            );
        }

        if (effect.colorMode == LedColorMode::Gradient) {
            const float blend = (static_cast<float>(i) + 0.5f) / static_cast<float>(SEGMENTS);
            red = ClampColor(static_cast<float>(red) * (1.0f - blend) + effect.secondaryRed * blend);
            green = ClampColor(static_cast<float>(green) * (1.0f - blend) + effect.secondaryGreen * blend);
            blue = ClampColor(static_cast<float>(blue) * (1.0f - blend) + effect.secondaryBlue * blend);
        } else if (effect.colorMode == LedColorMode::Rainbow &&
                   effect.type != LedEffectType::RGB) {
            HSVtoRGB(static_cast<float>(time) * 0.06f + static_cast<float>(i) * (360.0f / SEGMENTS), red, green, blue);
        }

        red = ClampColor(red * brightness * intensity);
        green = ClampColor(green * brightness * intensity);
        blue = ClampColor(blue * brightness * intensity);

        const float left =
            t.x - effectiveWidth * 0.5f + i * segmentWidth;

        const float right =
            left + segmentWidth * 0.88f;

        CVector local[4];

        local[0] = {left,  t.y, t.z - halfHeight};
        local[1] = {right, t.y, t.z - halfHeight};
        local[2] = {right, t.y, t.z + halfHeight};
        local[3] = {left,  t.y, t.z + halfHeight};

        for (uint32_t v = 0; v < 4; ++v) {
            CVector world;
            if (projectToWindshield) {
                CVector vehicleLocal = TransformPlacementLocalPoint(local[v], t);
                CVector projected;
                if (ProjectToSurfacePatch(vehicleLocal, *surfacePatch, *surfaceMesh, projected)) {
                    world = matrix->TransformPoint(projected);
                } else {
                    // A single failed corner must not cancel the whole LED.
                    world = TransformPlacementPoint(local[v], t, matrix);
                }
            } else {
                // Preserve the previously working placement path as fallback.
                world = TransformPlacementPoint(local[v], t, matrix);
            }

            SetVertex(
                vertices[i * 4 + v],
                world,
                red,
                green,
                blue,
                ClampColor(effect.opacity * 255.0f)
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
        if (!placement.enabled)
            continue;
        for (const LedEffectConfig& effect : placement.effects) {
            if (RenderEffect(vehicle, placement, effect))
                ++rendered;
        }
    }

    return rendered;
}

bool ProfileRenderer::RenderSelectedEffect(
    CVehicle* vehicle,
    const VehicleProfile& profile,
    uint32_t placementId,
    uint32_t effectIndex
) {
    if (vehicle == nullptr || !profile.enabled)
        return false;

    for (const LedPlacement& placement : profile.placements) {
        if (placement.id != placementId)
            continue;
        if (!placement.enabled)
            return false;

        if (effectIndex >= placement.effects.size())
            return false;

        return RenderEffect(
            vehicle,
            placement,
            placement.effects[effectIndex]
        );
    }

    return false;
}

}
