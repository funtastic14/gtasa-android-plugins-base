
#include "profile_renderer.h"
#include "matrix_animation_engine.h"
#include "led_motion_engine.h"
#include "effect_transition_engine.h"
#include "vehicle_geometry_reader.h"

#include <algorithm>
#include <cctype>
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
constexpr float NATIVE_RGB_SURFACE_OFFSET = 0.012f;

class RenderStateGuard {
public:
    explicit RenderStateGuard(
        LedLayerBlendMode blendMode = LedLayerBlendMode::Alpha,
        bool preserveTexture = false
    ) {
        Save(rwRENDERSTATETEXTURERASTER, m_texture, m_hasTexture);
        Save(rwRENDERSTATEVERTEXALPHAENABLE, m_vertexAlpha, m_hasVertexAlpha);
        Save(rwRENDERSTATESRCBLEND, m_srcBlend, m_hasSrcBlend);
        Save(rwRENDERSTATEDESTBLEND, m_destBlend, m_hasDestBlend);
        Save(rwRENDERSTATEZTESTENABLE, m_zTest, m_hasZTest);
        Save(rwRENDERSTATEZWRITEENABLE, m_zWrite, m_hasZWrite);
        Save(rwRENDERSTATECULLMODE, m_cullMode, m_hasCullMode);

        if (!preserveTexture)
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, Value(1));
        RwRenderStateSet(rwRENDERSTATESRCBLEND, Value(rwBLENDSRCALPHA));
        RwRenderStateSet(
            rwRENDERSTATEDESTBLEND,
            Value(blendMode == LedLayerBlendMode::Additive
                ? rwBLENDONE : rwBLENDINVSRCALPHA)
        );

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
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;

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


// Build a compact polygon mesh for one LED element. The placement/panel
// rectangle is unchanged; only the geometry inside each cell is shaped.
static void BuildElementGeometry(
    LedElementShape shape,
    float centerX,
    float centerZ,
    float halfWidth,
    float halfHeight,
    float y,
    CVector outVertices[32],
    RwImVertexIndex outIndices[96],
    uint32_t& vertexCount,
    uint32_t& indexCount
) {
    static const float unitX[16] = {
        1.0f, 0.92388f, 0.707107f, 0.382683f,
        0.0f, -0.382683f, -0.707107f, -0.92388f,
        -1.0f, -0.92388f, -0.707107f, -0.382683f,
        0.0f, 0.382683f, 0.707107f, 0.92388f
    };
    static const float unitZ[16] = {
        0.0f, 0.382683f, 0.707107f, 0.92388f,
        1.0f, 0.92388f, 0.707107f, 0.382683f,
        0.0f, -0.382683f, -0.707107f, -0.92388f,
        -1.0f, -0.92388f, -0.707107f, -0.382683f
    };
    vertexCount = 0;
    indexCount = 0;
    auto add = [&](float x, float z) {
        outVertices[vertexCount++] = CVector(centerX + x * halfWidth, y,
                                             centerZ + z * halfHeight);
    };
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        outIndices[indexCount++] = static_cast<RwImVertexIndex>(a);
        outIndices[indexCount++] = static_cast<RwImVertexIndex>(b);
        outIndices[indexCount++] = static_cast<RwImVertexIndex>(c);
    };
    if (shape == LedElementShape::Square) {
        add(-1.0f, -1.0f); add(1.0f, -1.0f);
        add(1.0f, 1.0f); add(-1.0f, 1.0f);
        triangle(0, 1, 2); triangle(0, 2, 3);
        return;
    }
    if (shape == LedElementShape::Diamond ||
        shape == LedElementShape::Triangle ||
        shape == LedElementShape::Hexagon ||
        shape == LedElementShape::Octagon) {
        uint32_t count = 4;
        static const uint8_t diamond[4] = {0, 4, 8, 12};
        static const uint8_t trianglePoints[3] = {0, 5, 11};
        static const uint8_t hexagon[6] = {0, 3, 5, 8, 11, 13};
        static const uint8_t octagon[8] = {0, 2, 4, 6, 8, 10, 12, 14};
        const uint8_t* points = diamond;
        if (shape == LedElementShape::Triangle) { count = 3; points = trianglePoints; }
        else if (shape == LedElementShape::Hexagon) { count = 6; points = hexagon; }
        else if (shape == LedElementShape::Octagon) { count = 8; points = octagon; }
        add(0.0f, 0.0f);
        for (uint32_t i = 0; i < count; ++i)
            add(unitX[points[i]], unitZ[points[i]]);
        for (uint32_t i = 0; i < count; ++i)
            triangle(0, i + 1, ((i + 1) % count) + 1);
        return;
    }
    if (shape == LedElementShape::CircleOutline) {
        for (uint32_t i = 0; i < 16; ++i) add(unitX[i], unitZ[i]);
        for (uint32_t i = 0; i < 16; ++i)
            add(unitX[i] * 0.68f, unitZ[i] * 0.68f);
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t next = (i + 1) % 16;
            triangle(i, next, 16 + next);
            triangle(i, 16 + next, 16 + i);
        }
        return;
    }
    // Filled circle (16-sided disc).
    add(0.0f, 0.0f);
    for (uint32_t i = 0; i < 16; ++i) add(unitX[i], unitZ[i]);
    for (uint32_t i = 0; i < 16; ++i)
        triangle(0, i + 1, ((i + 1) % 16) + 1);
}

static void AppendLightGeometry(
    std::vector<RwIm3DVertex>& vertices,
    std::vector<RwImVertexIndex>& indices,
    LedElementShape shape,
    float centerX, float centerZ, float halfWidth, float halfHeight, float y,
    const LedTransform& transform, const CMatrix* matrix,
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha
) {
    if (alpha == 0 || halfWidth <= 0.0f || halfHeight <= 0.0f ||
        vertices.size() + 32u > 65536u)
        return;
    CVector local[32];
    RwImVertexIndex localIndices[96];
    uint32_t vertexCount = 0, indexCount = 0;
    BuildElementGeometry(shape, centerX, centerZ, halfWidth, halfHeight, y,
        local, localIndices, vertexCount, indexCount);
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (uint32_t i = 0; i < vertexCount; ++i) {
        RwIm3DVertex vertex;
        SetVertex(vertex, TransformPlacementPoint(local[i], transform, matrix),
            red, green, blue, alpha);
        vertices.push_back(vertex);
    }
    for (uint32_t i = 0; i < indexCount; ++i)
        indices.push_back(static_cast<RwImVertexIndex>(base + localIndices[i]));
}

static MotionPlaybackMode GetPlacementMotionPlayback(const LedPlacement& placement) {
    switch (placement.animationMode) {
    case 1: return MotionPlaybackMode::Automatic;
    case 2: return MotionPlaybackMode::Random;
    case 3: return MotionPlaybackMode::Sequential;
    default: return MotionPlaybackMode::Manual;
    }
}

static bool RenderMatrixEffect(
    CVehicle* vehicle,
    const LedPlacement& placement,
    const LedEffectConfig& effect,
    uint32_t motionTimeMs,
    const MotionFrameState& motionFrame,
    float alphaCoverage = 1.0f,
    EffectTransitionMode transitionMode = EffectTransitionMode::Crossfade,
    EffectTransitionDirection transitionDirection = EffectTransitionDirection::LeftToRight,
    float transitionProgress = 1.0f,
    bool incomingTransition = true,
    uint32_t transitionSeed = 0
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

    // Transition masks use the same logical coordinates as the shared
    // animation canvas. This keeps wipes, shutters, radial reveals and pixel
    // dissolves continuous when one matrix is split across several regions.
    const bool useSharedCanvas =
        effect.matrix.canvasEnabled &&
        effect.matrix.canvasColumns > 0 &&
        effect.matrix.canvasRows > 0 &&
        static_cast<uint32_t>(effect.matrix.canvasOffsetColumn) + columns <=
            effect.matrix.canvasColumns &&
        static_cast<uint32_t>(effect.matrix.canvasOffsetRow) + rows <=
            effect.matrix.canvasRows;
    const uint16_t transitionColumns = useSharedCanvas
        ? effect.matrix.canvasColumns : static_cast<uint16_t>(columns);
    const uint16_t transitionRows = useSharedCanvas
        ? effect.matrix.canvasRows : static_cast<uint16_t>(rows);
    const uint16_t transitionOffsetColumn = useSharedCanvas
        ? effect.matrix.canvasOffsetColumn : 0u;
    const uint16_t transitionOffsetRow = useSharedCanvas
        ? effect.matrix.canvasOffsetRow : 0u;

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

    const float matrixGap = motionFrame.pattern == LedMotionPattern::Checkerboard
        ? 0.42f : 0.12f;
    const float gapX = cellWidth * matrixGap;
    const float gapZ = cellHeight * matrixGap;

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
    // Per-pixel shape selection means one matrix may mix simple and complex
    // meshes. Reserve bounded reusable capacity; the 16-bit index guard below
    // remains authoritative for unusually dense matrices.
    const size_t vertexReserve = std::min<size_t>(
        static_cast<size_t>(pixelCount) * 32u, 65536u);
    const size_t indexReserve = static_cast<size_t>(pixelCount) * 96u;
    if (vertices.capacity() < vertexReserve)
        vertices.reserve(vertexReserve);
    if (indices.capacity() < indexReserve)
        indices.reserve(indexReserve);

    // Resolve and decode the selected eye art once per placement, before
    // entering the per-pixel hot loop. Texture changes may load here, never
    // during each pixel evaluation.
    MatrixAnimationEngine::PrepareEffect(effect, motionTimeMs);

    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            MatrixPixelOutput output;

            if (!MatrixAnimationEngine::EvaluatePixel(
                    effect,
                    static_cast<uint16_t>(column),
                    static_cast<uint16_t>(row),
                    motionTimeMs,
                    output,
                    GetPlacementMotionPlayback(placement),
                    placement.animationIntervalMs,
                    &motionFrame
                ))
                return false;

            // Invisible pixels do not need vertices or triangles at all.
            if (!output.visible || output.intensity <= 0.001f)
                continue;

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
            float transitionCoverage = 1.0f;
            if (transitionProgress < 1.0f) {
                transitionCoverage = TransitionEngine::EvaluateIncoming(
                    transitionMode, transitionDirection, transitionProgress,
                    (static_cast<float>(transitionOffsetColumn + column) + 0.5f) /
                        static_cast<float>(transitionColumns),
                    (static_cast<float>(transitionOffsetRow + row) + 0.5f) /
                        static_cast<float>(transitionRows),
                    static_cast<uint16_t>(transitionOffsetColumn + column),
                    static_cast<uint16_t>(transitionOffsetRow + row),
                    transitionColumns, transitionRows, transitionSeed
                );
                if (!incomingTransition)
                    transitionCoverage = 1.0f - transitionCoverage;
            }

            const float centerX = (left + right) * 0.5f;
            const float centerZ = (top + bottom) * 0.5f;
            const float baseHalfWidth = (right - left) * 0.5f;
            const float baseHalfHeight = (top - bottom) * 0.5f;
            const uint8_t lightAlpha = ClampColor(output.alpha *
                TransitionEngine::Clamp01(alphaCoverage * transitionCoverage));
            if (effect.lightSpreadEnabled)
                AppendLightGeometry(vertices, indices, LedElementShape::Square,
                    centerX, centerZ, baseHalfWidth * 2.8f, baseHalfHeight * 2.8f,
                    t.y + 0.002f, t, matrix, red, green, blue,
                    ClampColor(35.0f * effect.lightSpreadIntensity * output.intensity *
                        TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));
            if (effect.glowEnabled) {
                AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                    centerX, centerZ, baseHalfWidth * 2.0f, baseHalfHeight * 2.0f,
                    t.y + 0.001f, t, matrix, red, green, blue,
                    ClampColor(65.0f * effect.glowIntensity * output.intensity *
                        TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));
                AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                    centerX, centerZ, baseHalfWidth * 1.35f, baseHalfHeight * 1.35f,
                    t.y + 0.0015f, t, matrix, red, green, blue,
                    ClampColor(85.0f * effect.glowIntensity * output.intensity *
                        TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));
            }
            if (effect.coronaEnabled)
                AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                    centerX, centerZ, baseHalfWidth * 1.45f, baseHalfHeight * 1.45f,
                    t.y + 0.003f, t, matrix, red, green, blue,
                    ClampColor(105.0f * effect.coronaIntensity * output.intensity *
                        TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));

            const uint32_t vertexBase = static_cast<uint32_t>(vertices.size());
            CVector local[32];
            RwImVertexIndex shapeIndices[96];
            uint32_t shapeVertexCount = 0;
            uint32_t shapeIndexCount = 0;
            const LedElementShape pixelShape =
                effect.matrix.pixels.size() == pixelCount &&
                effect.matrix.pixels[row * columns + column].elementShape != LedElementShape::Inherit
                    ? effect.matrix.pixels[row * columns + column].elementShape
                    : effect.elementShape;
            BuildElementGeometry(pixelShape,
                (left + right) * 0.5f, (top + bottom) * 0.5f,
                (right - left) * 0.5f, (top - bottom) * 0.5f, t.y,
                local, shapeIndices, shapeVertexCount, shapeIndexCount);
            // RenderWare uses 16-bit indices. Reject an oversized mesh safely
            // rather than allowing index wrap on very dense custom matrices.
            if (vertices.size() + shapeVertexCount > 65536u)
                return false;
            for (uint32_t v = 0; v < shapeVertexCount; ++v) {
                CVector world = TransformPlacementPoint(local[v], t, matrix);
                RwIm3DVertex vertex;
                SetVertex(vertex, world, red, green, blue,
                    ClampColor(output.alpha * TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));
                vertices.push_back(vertex);
            }
            for (uint32_t i = 0; i < shapeIndexCount; ++i)
                indices.push_back(static_cast<RwImVertexIndex>(vertexBase + shapeIndices[i]));
        }
    }

    if (vertices.empty() || indices.empty())
        return false;

    const bool hasLighting = effect.coronaEnabled || effect.glowEnabled || effect.lightSpreadEnabled;
    RenderStateGuard stateGuard(hasLighting ? LedLayerBlendMode::Additive : effect.blendMode);

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
    const LedEffectConfig& effect,
    uint32_t motionTimeMs,
    const MotionFrameState& motionFrame,
    float alphaCoverage = 1.0f,
    EffectTransitionMode transitionMode = EffectTransitionMode::Crossfade,
    EffectTransitionDirection transitionDirection = EffectTransitionDirection::LeftToRight,
    float transitionProgress = 1.0f,
    bool incomingTransition = true,
    uint32_t transitionSeed = 0
) {
    if (vehicle == nullptr || !effect.enabled)
        return false;

    if (effect.matrix.enabled)
        return RenderMatrixEffect(vehicle, placement, effect, motionTimeMs, motionFrame,
            alphaCoverage, transitionMode, transitionDirection, transitionProgress,
            incomingTransition, transitionSeed);

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

    // Reuse bounded buffers; non-square shapes can require up to 32 vertices
    // and 96 indices per LED (circle outline).
    static std::vector<RwIm3DVertex> vertices;
    static std::vector<RwImVertexIndex> indices;
    vertices.clear();
    indices.clear();
    if (vertices.capacity() < SEGMENTS * 32) vertices.reserve(SEGMENTS * 32);
    if (indices.capacity() < SEGMENTS * 96) indices.reserve(SEGMENTS * 96);

    const bool strobeOn = motionFrame.phase < 0.5f;

    // Strip effects can occupy a region of the same logical canvas used by
    // matrix effects. The physical geometry remains placement-local; only
    // animation/color sampling uses these shared logical coordinates.
    const LedMatrixConfig& stripCanvas = effect.matrix;
    const bool useSharedStripCanvas =
        stripCanvas.canvasEnabled &&
        stripCanvas.canvasColumns >= SEGMENTS &&
        stripCanvas.canvasRows > 0 &&
        static_cast<uint32_t>(stripCanvas.canvasOffsetColumn) + SEGMENTS <=
            stripCanvas.canvasColumns &&
        static_cast<uint32_t>(stripCanvas.canvasOffsetRow) <
            stripCanvas.canvasRows;
    const uint16_t logicalColumns = useSharedStripCanvas
        ? stripCanvas.canvasColumns : static_cast<uint16_t>(SEGMENTS);
    const uint16_t logicalRows = useSharedStripCanvas
        ? stripCanvas.canvasRows : 1u;
    const uint16_t logicalRow = useSharedStripCanvas
        ? stripCanvas.canvasOffsetRow : 0u;

    for (uint32_t i = 0; i < SEGMENTS; ++i) {
        const uint16_t logicalColumn = static_cast<uint16_t>(
            (useSharedStripCanvas ? stripCanvas.canvasOffsetColumn : 0u) + i
        );
        const float nx = (static_cast<float>(logicalColumn) + 0.5f) /
            static_cast<float>(logicalColumns);
        const float ny = (static_cast<float>(logicalRow) + 0.5f) /
            static_cast<float>(logicalRows);

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
            float colorPosition = nx;
            switch (effect.colorDirection) {
            case LedColorDirection::RightToLeft:
            case LedColorDirection::BottomToTop:
            case LedColorDirection::DiagonalUp:
                colorPosition = 1.0f - colorPosition;
                break;
            case LedColorDirection::CenterOut:
                colorPosition = std::fabs(colorPosition - 0.5f) * 2.0f;
                break;
            case LedColorDirection::EdgeToCenter:
                colorPosition = 1.0f - std::fabs(colorPosition - 0.5f) * 2.0f;
                break;
            default:
                break;
            }
            HSVtoRGB(
                motionFrame.phase * 360.0f +
                    colorPosition * effect.colorSpread * 360.0f,
                red, green, blue
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
            const float phase01 = motionFrame.phase;
            intensity *= EvaluateMotionIntensity(
                motionFrame.pattern, nx, ny, phase01,
                logicalColumn, logicalRow,
                logicalColumns, logicalRows
            );
        }

        if (effect.type != LedEffectType::RGB) {
            float position = nx;
            switch (effect.colorDirection) {
            case LedColorDirection::RightToLeft:
            case LedColorDirection::BottomToTop:
            case LedColorDirection::DiagonalUp:
                position = 1.0f - nx;
                break;
            case LedColorDirection::CenterOut:
                position = std::fabs(nx - 0.5f) * 2.0f;
                break;
            case LedColorDirection::EdgeToCenter:
                position = 1.0f - std::fabs(nx - 0.5f) * 2.0f;
                break;
            default:
                // A one-dimensional strip maps vertical/diagonal flow
                // onto its own longitudinal axis.
                position = nx;
                break;
            }
            const float spread = effect.colorSpread < 0.1f ? 0.1f : effect.colorSpread;
            const float rawPosition = position * spread;
            const float blend = rawPosition - std::floor(rawPosition);
            switch (effect.colorMode) {
            case LedColorMode::Gradient:
                red = ClampColor(static_cast<float>(red) * (1.0f - blend) + effect.secondaryRed * blend);
                green = ClampColor(static_cast<float>(green) * (1.0f - blend) + effect.secondaryGreen * blend);
                blue = ClampColor(static_cast<float>(blue) * (1.0f - blend) + effect.secondaryBlue * blend);
                break;
            case LedColorMode::ThreeColorGradient: {
                const float t = blend < 0.5f ? blend * 2.0f : (blend - 0.5f) * 2.0f;
                const float ar = blend < 0.5f ? effect.red : effect.secondaryRed;
                const float ag = blend < 0.5f ? effect.green : effect.secondaryGreen;
                const float ab = blend < 0.5f ? effect.blue : effect.secondaryBlue;
                const float br = blend < 0.5f ? effect.secondaryRed : effect.tertiaryRed;
                const float bg = blend < 0.5f ? effect.secondaryGreen : effect.tertiaryGreen;
                const float bb = blend < 0.5f ? effect.secondaryBlue : effect.tertiaryBlue;
                red = ClampColor(ar * (1.0f - t) + br * t);
                green = ClampColor(ag * (1.0f - t) + bg * t);
                blue = ClampColor(ab * (1.0f - t) + bb * t);
                break;
            }
            case LedColorMode::Rainbow:
                HSVtoRGB(motionFrame.phase * 360.0f + blend * 360.0f, red, green, blue);
                break;
            case LedColorMode::ColorCycle:
                HSVtoRGB(motionFrame.phase * 360.0f, red, green, blue);
                break;
            case LedColorMode::ColorWave: {
                const float wave = 0.5f + 0.5f * std::sin(
                    blend * 6.28318530718f * 2.0f - motionFrame.phase * 6.28318530718f
                );
                HSVtoRGB(wave * 360.0f + motionFrame.phase * 360.0f, red, green, blue);
                break;
            }
            case LedColorMode::ColorPulse: {
                const float pulse = 0.5f + 0.5f * std::sin(motionFrame.phase * 6.28318530718f);
                red = ClampColor(static_cast<float>(red) * (1.0f - pulse) + effect.secondaryRed * pulse);
                green = ClampColor(static_cast<float>(green) * (1.0f - pulse) + effect.secondaryGreen * pulse);
                blue = ClampColor(static_cast<float>(blue) * (1.0f - pulse) + effect.secondaryBlue * pulse);
                break;
            }
            case LedColorMode::Alternating: {
                const uint32_t band = static_cast<uint32_t>(
                    std::floor(blend * 8.0f + motionFrame.phase * 8.0f)
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

        red = ClampColor(red * brightness * intensity);
        green = ClampColor(green * brightness * intensity);
        blue = ClampColor(blue * brightness * intensity);
        float transitionCoverage = 1.0f;
        if (transitionProgress < 1.0f) {
            transitionCoverage = TransitionEngine::EvaluateIncoming(
                transitionMode, transitionDirection, transitionProgress,
                nx, ny, logicalColumn, logicalRow,
                logicalColumns, logicalRows, transitionSeed
            );
            if (!incomingTransition)
                transitionCoverage = 1.0f - transitionCoverage;
        }

        const float left =
            t.x - effectiveWidth * 0.5f + i * segmentWidth;

        const float right =
            left + segmentWidth * 0.88f;

        const float centerX = (left + right) * 0.5f;
        const float centerZ = t.z;
        const float halfWidth = (right - left) * 0.5f;
        const uint8_t lightAlpha = ClampColor(255.0f * intensity *
            effect.opacity * alphaCoverage * transitionCoverage);
        if (effect.lightSpreadEnabled)
            AppendLightGeometry(vertices, indices, LedElementShape::Square,
                centerX, centerZ, halfWidth * 2.8f, halfHeight * 2.8f,
                t.y + 0.002f, t, matrix, red, green, blue,
                ClampColor(35.0f * effect.lightSpreadIntensity * lightAlpha / 255.0f));
        if (effect.glowEnabled) {
            AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                centerX, centerZ, halfWidth * 2.0f, halfHeight * 2.0f,
                t.y + 0.001f, t, matrix, red, green, blue,
                ClampColor(65.0f * effect.glowIntensity * lightAlpha / 255.0f));
            AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                centerX, centerZ, halfWidth * 1.35f, halfHeight * 1.35f,
                t.y + 0.0015f, t, matrix, red, green, blue,
                ClampColor(85.0f * effect.glowIntensity * lightAlpha / 255.0f));
        }
        if (effect.coronaEnabled)
            AppendLightGeometry(vertices, indices, LedElementShape::CircleFilled,
                centerX, centerZ, halfWidth * 1.45f, halfHeight * 1.45f,
                t.y + 0.003f, t, matrix, red, green, blue,
                ClampColor(105.0f * effect.coronaIntensity * lightAlpha / 255.0f));

        CVector local[32];
        RwImVertexIndex shapeIndices[96];
        uint32_t shapeVertexCount = 0;
        uint32_t shapeIndexCount = 0;
        BuildElementGeometry(effect.elementShape,
            (left + right) * 0.5f, t.z,
            (right - left) * 0.5f, halfHeight, t.y,
            local, shapeIndices, shapeVertexCount, shapeIndexCount);
        const RwImVertexIndex base =
            static_cast<RwImVertexIndex>(vertices.size());
        for (uint32_t v = 0; v < shapeVertexCount; ++v) {
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
            RwIm3DVertex vertex;
            SetVertex(vertex, world, red, green, blue,
                ClampColor(effect.opacity * 255.0f *
                    TransitionEngine::Clamp01(alphaCoverage * transitionCoverage)));
            vertices.push_back(vertex);
        }
        for (uint32_t i = 0; i < shapeIndexCount; ++i)
            indices.push_back(static_cast<RwImVertexIndex>(base + shapeIndices[i]));
    }

    const bool hasLighting = effect.coronaEnabled || effect.glowEnabled || effect.lightSpreadEnabled;
    RenderStateGuard stateGuard(hasLighting ? LedLayerBlendMode::Additive : effect.blendMode);

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


static uint32_t RenderNativeLightsBatched(
    CVehicle* vehicle,
    const VehicleProfile& profile,
    const std::vector<AtomicMesh>& meshes,
    const std::vector<SurfacePatch>& patches,
    uint32_t timeMs,
    NativeLightRenderStats* stats
) {
    if (vehicle == nullptr || meshes.empty() || patches.empty())
        return 0;

    CMatrix* vehicleMatrix = vehicle->GetMatrix();
    if (vehicleMatrix == nullptr) {
        if (stats != nullptr) ++stats->missingVehicleMatrix;
        return 0;
    }

    // Atomic indices are bounded by the geometry scanner (0..127). Build a
    // fixed lookup table once per render call instead of linearly searching
    // the mesh vector for every lamp patch.
    const AtomicMesh* meshByAtomic[128] = {};
    for (const AtomicMesh& mesh : meshes) {
        if (mesh.atomicIndex < 128u)
            meshByAtomic[mesh.atomicIndex] = &mesh;
    }

    // One reusable batch replaces a separate RenderWare transform/draw call
    // for every lamp island. Keep each batch below the 16-bit index ceiling.
    static std::vector<RwIm3DVertex> vertices;
    static std::vector<RwImVertexIndex> indices;
    constexpr size_t MAX_BATCH_VERTICES = 60000u;
    if (vertices.capacity() < 24000u) vertices.reserve(24000u);
    if (indices.capacity() < 24000u) indices.reserve(24000u);
    vertices.clear();
    indices.clear();

    RenderStateGuard stateGuard(profile.nativeLights.blendMode);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, reinterpret_cast<void*>(1));
    RwRenderStateSet(rwRENDERSTATESRCBLEND, reinterpret_cast<void*>(rwBLENDSRCALPHA));
    // RenderStateGuard applies the selected native base-layer blend mode.
    // Alpha remains the backward-compatible default; Additive is opt-in.
    // Respect the vehicle depth buffer: the RGB overlay must not shine
    // through body panels. Keep depth writes disabled for transparency.
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, reinterpret_cast<void*>(1));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, reinterpret_cast<void*>(0));
    RwRenderStateSet(rwRENDERSTATECULLMODE, reinterpret_cast<void*>(rwCULLMODECULLNONE));

    uint32_t renderedPatches = 0;
    uint32_t batchPatchCount = 0;
    // Keep a small contribution from the original GTA lamp texture while
    // prioritizing the saturated RGB hue. Alpha blending avoids the white
    // color buildup caused by additive blending over bright lamp textures.
    const uint8_t alpha = ClampColor(profile.nativeLights.rgbFlowIntensity * 220.0f);
    if (stats != nullptr) stats->alpha = alpha;
    const float flowOffset = std::fmod(
        static_cast<float>(timeMs) * profile.nativeLights.rgbFlowSpeed * 0.09f,
        360.0f
    );

    const auto flushBatch = [&]() {
        if (indices.empty())
            return;

        const RwUInt32 flags = rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA;
        if (RwIm3DTransform(
                vertices.data(),
                static_cast<RwUInt32>(vertices.size()),
                nullptr,
                flags
            ) == nullptr) {
            if (stats != nullptr) ++stats->transformFailed;
        } else {
            const bool ok = RwIm3DRenderIndexedPrimitive(
                rwPRIMTYPETRILIST,
                indices.data(),
                static_cast<RwUInt32>(indices.size())
            );
            RwIm3DEnd();
            if (ok) {
                renderedPatches += batchPatchCount;
            } else if (stats != nullptr) {
                ++stats->drawFailed;
            }
        }
        vertices.clear();
        indices.clear();
        batchPatchCount = 0;
    };

    for (const SurfacePatch& patch : patches) {
        if (patch.role != SurfaceRoleCandidate::Lighting)
            continue;
        if (stats != nullptr) ++stats->lightingPatches;

        const bool front = patch.centroid.y > 0.25f;
        const bool rear = patch.centroid.y < -0.25f;
        if (front && stats != nullptr) ++stats->frontPatches;
        if (rear && stats != nullptr) ++stats->rearPatches;
        if (!front && !rear) {
            // A patch near the vehicle's longitudinal center is not a
            // front/rear lamp candidate. Keep it separate from user-disabled
            // headlight/taillight patches in diagnostics.
            if (stats != nullptr) ++stats->unclassifiedSidePatches;
            continue;
        }
        if ((front && !profile.nativeLights.rgbHeadlightsEnabled) ||
            (rear && !profile.nativeLights.rgbTaillightsEnabled)) {
            if (stats != nullptr) ++stats->sideDisabled;
            continue;
        }

        const AtomicMesh* mesh = patch.atomicIndex < 128u
            ? meshByAtomic[patch.atomicIndex] : nullptr;
        if (mesh == nullptr) {
            if (stats != nullptr) ++stats->missingMesh;
            continue;
        }
        if (mesh->vertices.empty() ||
            mesh->vehicleSpaceVertices.size() != mesh->vertices.size() ||
            mesh->vertices.size() > 65535u ||
            patch.triangleIndices.empty()) {
            if (stats != nullptr) ++stats->invalidGeometry;
            continue;
        }

        const float minX = patch.minBounds.x;
        const float width = std::max(0.001f, patch.maxBounds.x - minX);
        CVector outward(patch.normal.x, patch.normal.y, patch.normal.z);
        const float normalLength = std::sqrt(DotProduct(outward, outward));
        if (normalLength > 0.0001f) {
            outward = outward * (1.0f / normalLength);
            const CVector center(patch.centroid.x, patch.centroid.y, patch.centroid.z);
            if (DotProduct(outward, center) < 0.0f)
                outward = outward * -1.0f;
        } else {
            outward = CVector(0.0f, front ? 1.0f : -1.0f, 0.0f);
        }

        bool patchAdded = false;
        for (uint32_t triangleIndex : patch.triangleIndices) {
            if (triangleIndex >= mesh->triangles.size())
                continue;
            const GeometryTriangle& triangle = mesh->triangles[triangleIndex];
            if (triangle.materialIndex != patch.materialIndex ||
                triangle.a >= mesh->vertices.size() ||
                triangle.b >= mesh->vertices.size() ||
                triangle.c >= mesh->vertices.size())
                continue;

            if (stats != nullptr) {
                ++stats->validTriangles;
                stats->submittedVertices += 3u;
                stats->submittedIndices += 3u;
            }
            if (vertices.size() + 3u > MAX_BATCH_VERTICES)
                flushBatch();

            const uint16_t sourceIndices[3] = {
                triangle.a, triangle.b, triangle.c
            };

            // Use each triangle's own geometric normal instead of one
            // averaged patch normal. Curved lamp housings can contain faces
            // with different directions; a single average may push some
            // triangles back into the body and make only part of a lamp show.
            const GeometryVertex& la = mesh->vehicleSpaceVertices[triangle.a];
            const GeometryVertex& lb = mesh->vehicleSpaceVertices[triangle.b];
            const GeometryVertex& lc = mesh->vehicleSpaceVertices[triangle.c];
            const CVector edge1(lb.x - la.x, lb.y - la.y, lb.z - la.z);
            const CVector edge2(lc.x - la.x, lc.y - la.y, lc.z - la.z);
            CVector triangleOutward(
                edge1.y * edge2.z - edge1.z * edge2.y,
                edge1.z * edge2.x - edge1.x * edge2.z,
                edge1.x * edge2.y - edge1.y * edge2.x
            );
            const float triangleNormalLength = std::sqrt(
                DotProduct(triangleOutward, triangleOutward)
            );
            if (triangleNormalLength > 0.0001f) {
                triangleOutward = triangleOutward * (1.0f / triangleNormalLength);
                const CVector triangleCenter(
                    (la.x + lb.x + lc.x) / 3.0f,
                    (la.y + lb.y + lc.y) / 3.0f,
                    (la.z + lb.z + lc.z) / 3.0f
                );
                if (DotProduct(triangleOutward, triangleCenter) < 0.0f)
                    triangleOutward = triangleOutward * -1.0f;
            } else {
                triangleOutward = outward;
            }

            for (uint16_t sourceIndex : sourceIndices) {
                const GeometryVertex& source = mesh->vertices[sourceIndex];
                const GeometryVertex& local = mesh->vehicleSpaceVertices[sourceIndex];
                CVector vehicleLocal(local.x, local.y, local.z);
                vehicleLocal += triangleOutward * NATIVE_RGB_SURFACE_OFFSET;
                const CVector world = vehicleMatrix->TransformPoint(vehicleLocal);

                // One complete, fully saturated spectrum spans the lamp;
                // the time phase moves it continuously along the surface.
                const float normalizedX = (local.x - minX) / width;
                uint8_t red = 255, green = 255, blue = 255;
                HSVtoRGB(normalizedX * 360.0f + flowOffset, red, green, blue);

                RwIm3DVertex vertex;
                SetVertex(vertex, world, red, green, blue, alpha);
                vertex.u = source.u;
                vertex.v = source.v;
                const RwImVertexIndex next =
                    static_cast<RwImVertexIndex>(vertices.size());
                vertices.push_back(vertex);
                indices.push_back(next);
            }
            patchAdded = true;
        }
        if (patchAdded) {
            ++batchPatchCount;
        } else if (stats != nullptr) {
            // Count lighting patches that passed material/side filtering but
            // produced no valid triangle output. This keeps the on-device
            // diagnostic useful when a material group is empty or malformed.
            ++stats->emptyTriangleOutput;
        }
    }

    flushBatch();
    if (stats != nullptr)
        stats->renderedPatches += renderedPatches;
    return renderedPatches;
}
} // namespace


uint32_t ProfileRenderer::RenderNativeLights(
    CVehicle* vehicle,
    const VehicleProfile& profile,
    uint32_t frameTimeMs,
    NativeLightRenderStats* stats
) {
    if (vehicle == nullptr || !profile.enabled ||
        (!profile.nativeLights.rgbHeadlightsEnabled &&
         !profile.nativeLights.rgbTaillightsEnabled)) {
        return 0;
    }

    const std::vector<AtomicMesh>& meshes = VehicleGeometryReader::GetMeshes();
    const std::vector<SurfacePatch>& patches = VehicleGeometryReader::GetSurfacePatches();
    if (meshes.empty() || patches.empty())
        return 0;

    return RenderNativeLightsBatched(
        vehicle, profile, meshes, patches, frameTimeMs, stats
    );
}

uint32_t ProfileRenderer::RenderVehicleProfile(
    CVehicle* vehicle,
    const VehicleProfile& profile,
    uint32_t frameTimeMs,
    const EffectSequenceStep* activeStep,
    const EffectSequenceStep* previousStep,
    float transitionProgress,
    EffectTransitionMode transitionMode,
    EffectTransitionDirection transitionDirection
) {
    if (vehicle == nullptr || !profile.enabled)
        return 0;

    uint32_t rendered = 0;

    // The caller supplies one frame timestamp shared with Native RGB.

    for (const LedPlacement& placement : profile.placements) {
        if (!placement.enabled || placement.effects.empty())
            continue;

        // During a sequence step change, draw the outgoing effect first
        // and fade the incoming effect over it. Both use the same frame clock.
        const bool isOutgoing = previousStep != nullptr &&
            previousStep->placementId == placement.id &&
            previousStep->effectIndex < placement.effects.size();
        const bool isIncoming = activeStep != nullptr &&
            activeStep->placementId == placement.id &&
            activeStep->effectIndex < placement.effects.size();
        if (isOutgoing || isIncoming) {
            // Repeating the same effect in adjacent sequence steps is not a
            // visual change. Draw it once at full opacity instead of blending
            // two identical passes, which would otherwise dim alpha layers.
            if (isOutgoing && isIncoming &&
                previousStep->effectIndex == activeStep->effectIndex) {
                const LedEffectConfig& effect =
                    placement.effects[activeStep->effectIndex];
                const MotionFrameState frame = BuildMotionFrameState(
                    effect, frameTimeMs,
                    GetPlacementMotionPlayback(placement),
                    placement.animationIntervalMs
                );
                if (RenderEffect(vehicle, placement, effect, frameTimeMs, frame))
                    ++rendered;
                continue;
            }

            const uint32_t transitionSeed =
                (static_cast<uint32_t>(profile.modelId) * 2654435761u) ^
                (activeStep->placementId * 2246822519u) ^
                (activeStep->effectIndex * 3266489917u);
            if (isOutgoing) {
                const LedEffectConfig& outgoing =
                    placement.effects[previousStep->effectIndex];
                const MotionFrameState outgoingFrame = BuildMotionFrameState(
                    outgoing, frameTimeMs,
                    GetPlacementMotionPlayback(placement),
                    placement.animationIntervalMs
                );
                if (RenderEffect(vehicle, placement, outgoing, frameTimeMs,
                        outgoingFrame, 1.0f, transitionMode,
                        transitionDirection, transitionProgress, false,
                        transitionSeed))
                    ++rendered;
            }
            if (isIncoming) {
                const LedEffectConfig& incoming =
                    placement.effects[activeStep->effectIndex];
                const MotionFrameState incomingFrame = BuildMotionFrameState(
                    incoming, frameTimeMs,
                    GetPlacementMotionPlayback(placement),
                    placement.animationIntervalMs
                );
                if (RenderEffect(vehicle, placement, incoming, frameTimeMs,
                        incomingFrame, 1.0f, transitionMode,
                        transitionDirection, transitionProgress, true,
                        transitionSeed))
                    ++rendered;
            }
            continue;
        }

        // Draw effect layers in stable ascending order without allocating
        // a temporary sort buffer in the per-frame render path.
        bool hasPreviousLayer = false;
        uint8_t previousLayerOrder = 0;
        size_t previousLayerIndex = 0;
        for (size_t layer = 0; layer < placement.effects.size(); ++layer) {
            size_t selectedIndex = placement.effects.size();
            uint8_t selectedOrder = 255;
            for (size_t candidate = 0; candidate < placement.effects.size(); ++candidate) {
                const uint8_t order = placement.effects[candidate].layerOrder;
                const bool followsPrevious = !hasPreviousLayer ||
                    order > previousLayerOrder ||
                    (order == previousLayerOrder && candidate > previousLayerIndex);
                if (!followsPrevious)
                    continue;
                if (selectedIndex == placement.effects.size() || order < selectedOrder) {
                    selectedIndex = candidate;
                    selectedOrder = order;
                }
            }
            if (selectedIndex == placement.effects.size())
                break;
            const LedEffectConfig& layerEffect = placement.effects[selectedIndex];
            const MotionFrameState layerMotionFrame = BuildMotionFrameState(
                layerEffect, frameTimeMs, GetPlacementMotionPlayback(placement),
                placement.animationIntervalMs
            );
            if (RenderEffect(vehicle, placement, layerEffect,
                    frameTimeMs, layerMotionFrame))
                ++rendered;
            hasPreviousLayer = true;
            previousLayerOrder = selectedOrder;
            previousLayerIndex = selectedIndex;
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

        const uint32_t motionTimeMs = CTimer::GetTimeMS();
        const MotionFrameState motionFrame = BuildMotionFrameState(
            placement.effects[effectIndex], motionTimeMs,
            GetPlacementMotionPlayback(placement),
            placement.animationIntervalMs
        );
        return RenderEffect(
            vehicle,
            placement,
            placement.effects[effectIndex],
            motionTimeMs,
            motionFrame
        );
    }

    return false;
}

}
