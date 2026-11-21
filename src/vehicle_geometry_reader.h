
#pragma once

#include <cstdint>
#include <vector>

struct CVehicle;
struct RwFrame;

namespace VLS {

struct GeometryScanStats {
    uint32_t atomicCount = 0;
    uint32_t geometryCount = 0;

    uint64_t totalVertices = 0;
    uint64_t totalTriangles = 0;

    uint32_t cpuVertexGeometryCount = 0;
    uint32_t nativeGeometryCount = 0;
    uint32_t vertexBufferGeometryCount = 0;
    uint32_t missingVertexDataCount = 0;
    uint32_t missingTriangleDataCount = 0;
};

struct GeometryVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct GeometryTriangle {
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;
};

struct AtomicMesh {
    uint32_t atomicIndex = 0;
    uint32_t geometryIndex = 0;

    RwFrame* frame = nullptr;

    std::vector<GeometryVertex> vertices;
    std::vector<GeometryTriangle> triangles;
};

class VehicleGeometryReader {
public:
    // Membaca metadata dan menyimpan mesh CPU kendaraan aktif.
    static bool Scan(
        CVehicle* vehicle,
        int modelId,
        GeometryScanStats& stats
    );

    // Mengakses mesh kendaraan yang terakhir dipindai.
    static const std::vector<AtomicMesh>& GetMeshes();

    // Menghapus cache mesh kendaraan.
    static void Clear();
};

} // namespace VLS
