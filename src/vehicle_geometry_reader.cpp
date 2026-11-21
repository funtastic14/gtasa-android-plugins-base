#include "vehicle_geometry_reader.h"

#include "profile_storage.h"

#include <mod/logger.h>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/renderware/RpClump.h>
#include <plugin-sdk/renderware/RpAtomic.h>
#include <plugin-sdk/renderware/RpGeometry.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace VLS {

namespace {

struct AtomicGeometryInfo {
    uint32_t atomicIndex = 0;
    uint32_t geometryIndex = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    bool hasCpuVertices = false;
    uint32_t validVertexCount = 0;
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
};

struct ScanContext {
    GeometryScanStats* stats = nullptr;
    std::vector<AtomicGeometryInfo>* details = nullptr;
    uint32_t geometryIndex = 0;
};

static int g_lastSavedModelId = -2147483647;

static bool IsFiniteVertex(const RwV3d& vertex) {
    return std::isfinite(vertex.x) &&
           std::isfinite(vertex.y) &&
           std::isfinite(vertex.z);
}

static RpAtomic* ScanAtomic(RpAtomic* atomic, void* data) {
    if (atomic == nullptr || data == nullptr)
        return atomic;

    ScanContext* context = static_cast<ScanContext*>(data);
    if (context->stats == nullptr || context->details == nullptr)
        return atomic;

    GeometryScanStats& stats = *context->stats;
    const uint32_t atomicIndex = stats.atomicCount++;

    RpGeometry* geometry = RpAtomicGetGeometry(atomic);
    if (geometry == nullptr)
        return atomic;

    const uint32_t geometryIndex = context->geometryIndex++;
    ++stats.geometryCount;

    const RwInt32 signedVertexCount = RpGeometryGetNumVertices(geometry);
    const RwInt32 signedTriangleCount = RpGeometryGetNumTriangles(geometry);
    const RwUInt32 flags = RpGeometryGetFlags(geometry);

    const uint32_t vertexCount = signedVertexCount > 0
        ? static_cast<uint32_t>(signedVertexCount) : 0;
    const uint32_t triangleCount = signedTriangleCount > 0
        ? static_cast<uint32_t>(signedTriangleCount) : 0;

    stats.totalVertices += vertexCount;
    stats.totalTriangles += triangleCount;

    const bool hasPositions = (flags & rpGEOMETRYPOSITIONS) != 0;
    const bool isNative = (flags & rpGEOMETRYNATIVE) != 0;
    const bool hasNativeInstance = (flags & rpGEOMETRYNATIVEINSTANCE) != 0;

    if (isNative || hasNativeInstance)
        ++stats.nativeGeometryCount;
    if (geometry->vertexBuffer != 0)
        ++stats.vertexBufferGeometryCount;

    AtomicGeometryInfo info;
    info.atomicIndex = atomicIndex;
    info.geometryIndex = geometryIndex;
    info.vertexCount = vertexCount;
    info.triangleCount = triangleCount;

    const RwV3d* vertices = nullptr;
    if (hasPositions && !isNative && !hasNativeInstance &&
        vertexCount > 0 && geometry->numMorphTargets > 0 &&
        geometry->morphTarget != nullptr) {
        const RpMorphTarget* morphTarget = RpGeometryGetMorphTarget(geometry, 0);
        if (morphTarget != nullptr)
            vertices = RpMorphTargetGetVertices(morphTarget);
    }

    if (vertices != nullptr) {
        info.hasCpuVertices = true;

        const float highest = std::numeric_limits<float>::max();
        info.minX = highest;
        info.minY = highest;
        info.minZ = highest;
        info.maxX = -highest;
        info.maxY = -highest;
        info.maxZ = -highest;

        // Batas defensif agar data rusak tidak memicu pembacaan tak terkendali.
        const uint32_t safeVertexCount = vertexCount > 1000000u
            ? 1000000u : vertexCount;

        for (uint32_t i = 0; i < safeVertexCount; ++i) {
            const RwV3d& vertex = vertices[i];
            if (!IsFiniteVertex(vertex))
                continue;

            if (vertex.x < info.minX) info.minX = vertex.x;
            if (vertex.y < info.minY) info.minY = vertex.y;
            if (vertex.z < info.minZ) info.minZ = vertex.z;
            if (vertex.x > info.maxX) info.maxX = vertex.x;
            if (vertex.y > info.maxY) info.maxY = vertex.y;
            if (vertex.z > info.maxZ) info.maxZ = vertex.z;
            ++info.validVertexCount;
        }

        if (info.validVertexCount == 0)
            info.hasCpuVertices = false;
    }

    if (info.hasCpuVertices) {
        ++stats.cpuVertexGeometryCount;
    } else {
        ++stats.missingVertexDataCount;
    }

    if (triangleCount > 0 && RpGeometryGetTriangles(geometry) == nullptr)
        ++stats.missingTriangleDataCount;

    context->details->push_back(info);
    return atomic;
}

static std::string BuildDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats,
    const std::vector<AtomicGeometryInfo>& details
) {
    std::ostringstream report;
    report << std::fixed << std::setprecision(4);

    report << "========================================\n";
    report << "VLS VEHICLE GEOMETRY DIAGNOSTIC\n";
    report << "========================================\n\n";
    report << "Model ID: " << modelId << "\n\n";
    report << "Atomic Count: " << stats.atomicCount << "\n";
    report << "Geometry Count: " << stats.geometryCount << "\n";
    report << "Total Vertices: " << static_cast<unsigned long long>(stats.totalVertices) << "\n";
    report << "Total Triangles: " << static_cast<unsigned long long>(stats.totalTriangles) << "\n\n";
    report << "CPU Vertex Geometry: " << stats.cpuVertexGeometryCount << "\n";
    report << "Native Geometry: " << stats.nativeGeometryCount << "\n";
    report << "Vertex Buffer Geometry: " << stats.vertexBufferGeometryCount << "\n";
    report << "Missing Vertex Data: " << stats.missingVertexDataCount << "\n";
    report << "Missing Triangle Data: " << stats.missingTriangleDataCount << "\n\n";

    report << "----------------------------------------\n";
    report << "PER-ATOMIC LOCAL GEOMETRY BOUNDS\n";
    report << "Coordinates below are geometry-local, not vehicle/world coordinates.\n";
    report << "----------------------------------------\n";

    for (const AtomicGeometryInfo& info : details) {
        report << "\nAtomic #" << info.atomicIndex
               << " | Geometry #" << info.geometryIndex << "\n";
        report << "Vertices: " << info.vertexCount
               << " | Triangles: " << info.triangleCount << "\n";

        if (!info.hasCpuVertices || info.validVertexCount == 0) {
            report << "Bounds: unavailable (CPU vertex positions unavailable)\n";
            continue;
        }

        report << "Valid Vertices: " << info.validVertexCount << "\n";
        report << "Min: X=" << info.minX << " Y=" << info.minY << " Z=" << info.minZ << "\n";
        report << "Max: X=" << info.maxX << " Y=" << info.maxY << " Z=" << info.maxZ << "\n";
        report << "Size: X=" << (info.maxX - info.minX)
               << " Y=" << (info.maxY - info.minY)
               << " Z=" << (info.maxZ - info.minZ) << "\n";
    }

    report << "\n========================================\n";
    report << "END OF DIAGNOSTIC\n";
    report << "========================================\n";
    return report.str();
}

static void SaveDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats,
    const std::vector<AtomicGeometryInfo>& details
) {
    if (g_lastSavedModelId == modelId)
        return;

    char fileName[64];
    if (modelId < 0) {
        std::snprintf(fileName, sizeof(fileName), "geometry_scan_unknown.txt");
    } else {
        std::snprintf(fileName, sizeof(fileName), "geometry_scan_%d.txt", modelId);
    }

    const std::string report = BuildDiagnosticReport(modelId, stats, details);
    if (ProfileStorage::SaveDiagnosticText(fileName, report)) {
        g_lastSavedModelId = modelId;
        logger->Info("[VLS] Geometry diagnostic saved: %s", fileName);
    } else {
        logger->Error("[VLS] Failed to save geometry diagnostic: %s", fileName);
    }
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

    RpClump* clump = vehicle->m_pRwClump;
    if (clump == nullptr) {
        logger->Info("[VLS] Geometry scan deferred: vehicle clump unavailable.");
        return false;
    }

    std::vector<AtomicGeometryInfo> details;
    ScanContext context;
    context.stats = &stats;
    context.details = &details;

    RpClumpForAllAtomics(clump, ScanAtomic, &context);

    logger->Info("[VLS] Vehicle Geometry Scan");
    logger->Info("[VLS] Model ID: %d", modelId);
    logger->Info("[VLS] Atomic Count: %u", static_cast<unsigned int>(stats.atomicCount));
    logger->Info("[VLS] Geometry Count: %u", static_cast<unsigned int>(stats.geometryCount));
    logger->Info("[VLS] Total Vertices: %llu", static_cast<unsigned long long>(stats.totalVertices));
    logger->Info("[VLS] Total Triangles: %llu", static_cast<unsigned long long>(stats.totalTriangles));
    logger->Info("[VLS] CPU Vertex Geometry: %u", static_cast<unsigned int>(stats.cpuVertexGeometryCount));
    logger->Info("[VLS] Native Geometry: %u", static_cast<unsigned int>(stats.nativeGeometryCount));
    logger->Info("[VLS] Vertex Buffer Geometry: %u", static_cast<unsigned int>(stats.vertexBufferGeometryCount));
    logger->Info("[VLS] Missing Vertex Data: %u", static_cast<unsigned int>(stats.missingVertexDataCount));
    logger->Info("[VLS] Missing Triangle Data: %u", static_cast<unsigned int>(stats.missingTriangleDataCount));

    SaveDiagnosticReport(modelId, stats, details);
    return stats.atomicCount > 0;
}

} // namespace VLS
