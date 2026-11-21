
#include "vehicle_geometry_reader.h"

#include "profile_storage.h"

#include <mod/logger.h>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/renderware/RpClump.h>
#include <plugin-sdk/renderware/RpAtomic.h>
#include <plugin-sdk/renderware/RpGeometry.h>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace VLS {

namespace {

struct ScanContext {
    GeometryScanStats* stats = nullptr;
};

static int g_lastSavedModelId = -2147483647;

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

    if (vertexCount > 0) {
        stats.totalVertices +=
            static_cast<uint32_t>(vertexCount);
    }

    if (triangleCount > 0) {
        stats.totalTriangles +=
            static_cast<uint32_t>(triangleCount);
    }

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

static std::string BuildDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats
) {
    std::ostringstream report;

    report << "========================================\n";
    report << "VLS VEHICLE GEOMETRY DIAGNOSTIC\n";
    report << "========================================\n\n";

    report << "Model ID: " << modelId << "\n\n";

    report << "Atomic Count: "
           << stats.atomicCount << "\n";

    report << "Geometry Count: "
           << stats.geometryCount << "\n";

    report << "Total Vertices: "
           << static_cast<unsigned long long>(
                stats.totalVertices
              ) << "\n";

    report << "Total Triangles: "
           << static_cast<unsigned long long>(
                stats.totalTriangles
              ) << "\n\n";

    report << "CPU Vertex Geometry: "
           << stats.cpuVertexGeometryCount << "\n";

    report << "Native Geometry: "
           << stats.nativeGeometryCount << "\n";

    report << "Vertex Buffer Geometry: "
           << stats.vertexBufferGeometryCount << "\n";

    report << "Missing Vertex Data: "
           << stats.missingVertexDataCount << "\n";

    report << "Missing Triangle Data: "
           << stats.missingTriangleDataCount << "\n";

    report << "\n========================================\n";
    report << "END OF DIAGNOSTIC\n";
    report << "========================================\n";

    return report.str();
}

static void SaveDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats
) {
    if (g_lastSavedModelId == modelId)
        return;

    char fileName[64];

    if (modelId < 0) {
        std::snprintf(
            fileName,
            sizeof(fileName),
            "geometry_scan_unknown.txt"
        );
    } else {
        std::snprintf(
            fileName,
            sizeof(fileName),
            "geometry_scan_%d.txt",
            modelId
        );
    }

    const std::string report =
        BuildDiagnosticReport(modelId, stats);

    if (ProfileStorage::SaveDiagnosticText(
        fileName,
        report
    )) {
        g_lastSavedModelId = modelId;

        logger->Info(
            "[VLS] Geometry diagnostic saved: %s",
            fileName
        );
    } else {
        logger->Error(
            "[VLS] Failed to save geometry diagnostic: %s",
            fileName
        );
    }
}

}

bool VehicleGeometryReader::Scan(
    CVehicle* vehicle,
    int modelId,
    GeometryScanStats& stats
) {
    stats = GeometryScanStats{};

    if (vehicle == nullptr)
        return false;

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

    logger->Info("[VLS] Vehicle Geometry Scan");

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

    SaveDiagnosticReport(modelId, stats);

    return stats.atomicCount > 0;
}

}
