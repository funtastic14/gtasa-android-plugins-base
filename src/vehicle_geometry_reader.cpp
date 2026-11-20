#include "vehicle_geometry_reader.h"

#include <mod/logger.h>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/renderware/RpClump.h>
#include <plugin-sdk/renderware/RpAtomic.h>
#include <plugin-sdk/renderware/RpGeometry.h>

#include <cstdint>

namespace VLS {

namespace {

struct ScanContext {
    GeometryScanStats* stats = nullptr;
};

static RpAtomic* ScanAtomic(
    RpAtomic* atomic,
    void* data
) {
    if (atomic == nullptr || data == nullptr)
        return atomic;

    ScanContext* context =
        static_cast<ScanContext*>(data);

    if (context->stats == nullptr)
        return atomic;

    GeometryScanStats& stats = *context->stats;

    ++stats.atomicCount;

    RpGeometry* geometry =
        RpAtomicGetGeometry(atomic);

    if (geometry == nullptr)
        return atomic;

    ++stats.geometryCount;

    const RwInt32 vertexCount =
        RpGeometryGetNumVertices(geometry);

    const RwInt32 triangleCount =
        RpGeometryGetNumTriangles(geometry);

    const RwUInt32 flags =
        RpGeometryGetFlags(geometry);

    if (vertexCount > 0)
        stats.totalVertices +=
            static_cast<uint32_t>(vertexCount);

    if (triangleCount > 0)
        stats.totalTriangles +=
            static_cast<uint32_t>(triangleCount);

    const bool hasPositions =
        (flags & rpGEOMETRYPOSITIONS) != 0;

    const bool isNative =
        (flags & rpGEOMETRYNATIVE) != 0;

    const bool hasNativeInstance =
        (flags & rpGEOMETRYNATIVEINSTANCE) != 0;

    if (isNative || hasNativeInstance)
        ++stats.nativeGeometryCount;

    if (geometry->vertexBuffer != 0)
        ++stats.vertexBufferGeometryCount;

    bool hasCpuVertices = false;

    if (hasPositions &&
        vertexCount > 0 &&
        geometry->numMorphTargets > 0 &&
        geometry->morphTarget != nullptr) {

        const RpMorphTarget* morphTarget =
            RpGeometryGetMorphTarget(geometry, 0);

        if (morphTarget != nullptr &&
            RpMorphTargetGetVertices(morphTarget) != nullptr) {
            hasCpuVertices = true;
        }
    }

    if (hasCpuVertices) {
        ++stats.cpuVertexGeometryCount;
    } else {
        ++stats.missingVertexDataCount;
    }

    if (triangleCount > 0 &&
        RpGeometryGetTriangles(geometry) == nullptr) {
        ++stats.missingTriangleDataCount;
    }

    return atomic;
}

} // namespace


bool VehicleGeometryReader::Scan(
    CVehicle* vehicle,
    int modelId,
    GeometryScanStats& stats
) {
    stats = GeometryScanStats{};

    if (vehicle == nullptr)
        return false;

    // Kendaraan GTA SA umumnya menggunakan RpClump.
    // Tetap periksa pointer sebelum melakukan traversal.
    RpClump* clump = vehicle->m_pRwClump;

    if (clump == nullptr) {
        logger->Info(
            "[VLS] Geometry scan deferred: vehicle clump unavailable."
        );
        return false;
    }

    ScanContext context;
    context.stats = &stats;

    RpClumpForAllAtomics(
        clump,
        ScanAtomic,
        &context
    );

    logger->Info(
        "[VLS] Vehicle Geometry Scan"
    );

    logger->Info(
        "[VLS] Model ID: %d",
        modelId
    );

    logger->Info(
        "[VLS] Atomic Count: %u",
        static_cast<unsigned int>(stats.atomicCount)
    );

    logger->Info(
        "[VLS] Geometry Count: %u",
        static_cast<unsigned int>(stats.geometryCount)
    );

    logger->Info(
        "[VLS] Total Vertices: %llu",
        static_cast<unsigned long long>(stats.totalVertices)
    );

    logger->Info(
        "[VLS] Total Triangles: %llu",
        static_cast<unsigned long long>(stats.totalTriangles)
    );

    logger->Info(
        "[VLS] CPU Vertex Geometry: %u",
        static_cast<unsigned int>(stats.cpuVertexGeometryCount)
    );

    logger->Info(
        "[VLS] Native Geometry: %u",
        static_cast<unsigned int>(stats.nativeGeometryCount)
    );

    logger->Info(
        "[VLS] Vertex Buffer Geometry: %u",
        static_cast<unsigned int>(stats.vertexBufferGeometryCount)
    );

    logger->Info(
        "[VLS] Missing Vertex Data: %u",
        static_cast<unsigned int>(stats.missingVertexDataCount)
    );

    logger->Info(
        "[VLS] Missing Triangle Data: %u",
        static_cast<unsigned int>(stats.missingTriangleDataCount)
    );

    return stats.atomicCount > 0;
}

} // namespace VLS
