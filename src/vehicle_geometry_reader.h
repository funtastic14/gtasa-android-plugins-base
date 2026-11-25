
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct CVehicle;
struct RwFrame;
struct RwRaster;

namespace VLS {

struct GeometryScanStats {
    char failureReason[128] = {};
    uint32_t atomicCount = 0;
    uint32_t geometryCount = 0;

    uint64_t totalVertices = 0;
    uint64_t totalTriangles = 0;

    uint32_t cpuVertexGeometryCount = 0;
    uint32_t nativeGeometryCount = 0;
    uint32_t vertexBufferGeometryCount = 0;
    uint32_t missingVertexDataCount = 0;
    uint32_t missingTriangleDataCount = 0;

    // Mesh cache statistics
    uint32_t cachedMeshCount = 0;
    uint64_t cachedVertexCount = 0;
    uint64_t cachedTriangleCount = 0;

    // Skipped mesh statistics
    uint32_t skippedNoCpuVertexCount = 0;
    uint32_t skippedVertexLimitCount = 0;
    uint32_t skippedNoTriangleDataCount = 0;
    uint32_t skippedInvalidVertexCount = 0;
    uint32_t skippedNoValidTriangleCount = 0;

    uint64_t invalidTriangleIndexCount = 0;
};

struct GeometryVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // Original RenderWare texture coordinates, when CPU UV data is available.
    float u = 0.0f;
    float v = 0.0f;
};

struct GeometryTriangle {
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    int16_t materialIndex = -1;
};

struct MeshMaterialInfo {
    int16_t index = -1;
    std::string textureName;
    std::string maskName;
    // Borrowed from the live model material; VLS never modifies or releases it.
    ::RwRaster* raster = nullptr;
};

enum class SurfaceRoleCandidate : uint8_t {
    Unknown,
    BodyChassis,
    FrontGlass,
    RearGlass,
    SideGlass,
    GlassUnresolved,
    Lighting
};

struct SurfacePatch {
    uint32_t atomicIndex = 0;
    int16_t materialIndex = -1;
    std::string frameName;
    std::string textureName;
    std::string maskName;
    SurfaceRoleCandidate role = SurfaceRoleCandidate::Unknown;
    std::vector<uint32_t> triangleIndices;
    GeometryVertex centroid;
    GeometryVertex normal;
    GeometryVertex minBounds;
    GeometryVertex maxBounds;
    float area = 0.0f;
};

struct AtomicMesh {
    uint32_t atomicIndex = 0;
    uint32_t geometryIndex = 0;
    // Numeric identity only; the RenderWare geometry is always re-resolved.
    uintptr_t geometryIdentity = 0;

    RwFrame* frame = nullptr;

    // Vertex coordinates transformed into vehicle-local space during Scan().
    // Valid for surface sampling/attachment; original vertices remain unchanged.
    std::vector<GeometryVertex> vertices;
    bool hasTextureCoordinates = false;
    std::vector<GeometryVertex> vehicleSpaceVertices;
    std::vector<GeometryTriangle> triangles;
    std::vector<MeshMaterialInfo> materials;
};

class VehicleGeometryReader {
public:
    // Membaca metadata dan menyimpan mesh CPU kendaraan aktif.
    static bool Scan(
        CVehicle* vehicle,
        int modelId,
        GeometryScanStats& stats
    );

    // Advances a bounded CPU-mesh copy on the game thread.
    // complete becomes true on success or a fail-closed rejection.
    static bool ScanIncremental(
        CVehicle* vehicle,
        int modelId,
        GeometryScanStats& stats,
        bool& complete
    );

    // Mengakses mesh kendaraan yang terakhir dipindai.
    static const std::vector<AtomicMesh>& GetMeshes();

    // Surface components derived from the scanned vehicle mesh.
    static const std::vector<SurfacePatch>& GetSurfacePatches();

    // Menghapus cache mesh kendaraan.
    static void Clear();
};

} // namespace VLS
