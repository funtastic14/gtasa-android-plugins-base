#pragma once

#include <cstdint>

struct CVehicle;

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

class VehicleGeometryReader {
public:
    // Hanya membaca metadata. Tidak mengunci atau mengubah geometri.
    static bool Scan(
        CVehicle* vehicle,
        int modelId,
        GeometryScanStats& stats
    );
};

}
