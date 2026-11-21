
#include "vehicle_geometry_reader.h"

#include "profile_storage.h"

#include <mod/logger.h>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/renderware/RpClump.h>
#include <plugin-sdk/renderware/RpAtomic.h>
#include <plugin-sdk/renderware/RpGeometry.h>
#include <plugin-sdk/renderware/RwFrame.h>
#include <plugin-sdk/renderware/RwMatrix.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VLS {

namespace {

static const size_t MAX_FRAME_COUNT = 4096;
static const size_t MAX_DIAGNOSTIC_OUTPUT = 240 * 1024;
static const uint32_t MAX_FRAME_DEPTH = 128;

struct AtomicGeometryInfo {
    uint32_t atomicIndex = 0;
    uint32_t geometryIndex = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;

    RwFrame* frame = nullptr;

    bool hasCpuVertices = false;
    uint32_t validVertexCount = 0;

    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;

    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
};

struct FrameInfo {
    RwFrame* frame = nullptr;
    RwFrame* parent = nullptr;

    uint32_t index = 0;
    uint32_t depth = 0;

    std::string name;

    RwV3d localPosition{};
    RwV3d worldPosition{};

    RwV3d right{};
    RwV3d up{};
    RwV3d at{};
};

struct ScanContext {
    GeometryScanStats* stats = nullptr;
    std::vector<AtomicGeometryInfo>* details = nullptr;
    uint32_t geometryIndex = 0;
};

struct FrameScanContext {
    std::vector<FrameInfo>* frames = nullptr;
    std::unordered_set<RwFrame*> visited;

    bool truncated = false;
    bool depthLimited = false;
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

    const RwInt32 signedVertexCount =
        RpGeometryGetNumVertices(geometry);

    const RwInt32 signedTriangleCount =
        RpGeometryGetNumTriangles(geometry);

    const RwUInt32 flags = RpGeometryGetFlags(geometry);

    const uint32_t vertexCount =
        signedVertexCount > 0
            ? static_cast<uint32_t>(signedVertexCount)
            : 0;

    const uint32_t triangleCount =
        signedTriangleCount > 0
            ? static_cast<uint32_t>(signedTriangleCount)
            : 0;

    stats.totalVertices += vertexCount;
    stats.totalTriangles += triangleCount;

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

    AtomicGeometryInfo info;

    info.atomicIndex = atomicIndex;
    info.geometryIndex = geometryIndex;
    info.vertexCount = vertexCount;
    info.triangleCount = triangleCount;
    info.frame = RpAtomicGetFrame(atomic);

    const RwV3d* vertices = nullptr;

    if (hasPositions &&
        !isNative &&
        !hasNativeInstance &&
        vertexCount > 0 &&
        geometry->numMorphTargets > 0 &&
        geometry->morphTarget != nullptr) {

        const RpMorphTarget* morphTarget =
            RpGeometryGetMorphTarget(geometry, 0);

        if (morphTarget != nullptr)
            vertices = RpMorphTargetGetVertices(morphTarget);
    }

    if (vertices != nullptr) {
        info.hasCpuVertices = true;

        const float highest =
            std::numeric_limits<float>::max();

        info.minX = highest;
        info.minY = highest;
        info.minZ = highest;

        info.maxX = -highest;
        info.maxY = -highest;
        info.maxZ = -highest;

        const uint32_t safeVertexCount =
            vertexCount > 1000000u
                ? 1000000u
                : vertexCount;

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

    if (triangleCount > 0 &&
        RpGeometryGetTriangles(geometry) == nullptr) {
        ++stats.missingTriangleDataCount;
    }

    context->details->push_back(info);

    return atomic;
}

// Membaca hierarki frame secara read-only.
// Setiap frame hanya diproses satu kali untuk menghindari siklus.
static void ScanFrameBranch(
    RwFrame* firstFrame,
    uint32_t depth,
    FrameScanContext& context
) {
    if (firstFrame == nullptr)
        return;

    if (context.frames == nullptr)
        return;

    if (depth > MAX_FRAME_DEPTH) {
        context.depthLimited = true;
        return;
    }

    for (RwFrame* frame = firstFrame;
         frame != nullptr;
         frame = frame->next) {

        if (context.frames->size() >= MAX_FRAME_COUNT) {
            context.truncated = true;
            return;
        }

        if (!context.visited.insert(frame).second) {
            // Frame đã visited: berhenti untuk mencegah sibling cycle.
            break;
        }

        FrameInfo info;

        info.frame = frame;
        info.parent = RwFrameGetParent(frame);
        info.index =
            static_cast<uint32_t>(context.frames->size());

        info.depth = depth;

        const char* nodeName = GetFrameNodeName(frame);

        info.name =
            (nodeName != nullptr && nodeName[0] != '\0')
                ? nodeName
                : "<unnamed>";

        // Salin matriks. Tidak memodifikasi frame atau matriks asli.
        const RwMatrix& modelling = frame->modelling;
        const RwMatrix& ltm = frame->ltm;

        info.localPosition = modelling.pos;
        info.worldPosition = ltm.pos;

        info.right = ltm.right;
        info.up = ltm.up;
        info.at = ltm.at;

        context.frames->push_back(info);

        // Telusuri anak sebelum melanjutkan sibling.
        if (frame->child != nullptr) {
            ScanFrameBranch(
                frame->child,
                depth + 1,
                context
            );
        }

        if (context.truncated)
            return;
    }
}

static void WriteVector(
    std::ostringstream& output,
    const RwV3d& value
) {
    output << "X=" << value.x
           << " Y=" << value.y
           << " Z=" << value.z;
}

static std::string BuildDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats,
    const std::vector<AtomicGeometryInfo>& details,
    const std::vector<FrameInfo>& frames,
    bool frameTruncated,
    bool depthLimited
) {
    std::ostringstream report;

    report << std::fixed << std::setprecision(4);

    report << "========================================\n";
    report << "VLS VEHICLE GEOMETRY DIAGNOSTIC\n";
    report << "========================================\n\n";

    report << "Model ID: " << modelId << "\n\n";

    report << "ATOMIC / GEOMETRY STATISTICS\n";
    report << "----------------------------------------\n";

    report << "Atomic Count: " << stats.atomicCount << "\n";
    report << "Geometry Count: " << stats.geometryCount << "\n";

    report << "Total Vertices: "
           << static_cast<unsigned long long>(
                  stats.totalVertices
              ) << "\n";

    report << "Total Triangles: "
           << static_cast<unsigned long long>(
                  stats.totalTriangles
              ) << "\n";

    report << "CPU Vertex Geometry: "
           << stats.cpuVertexGeometryCount << "\n";

    report << "Native Geometry: "
           << stats.nativeGeometryCount << "\n";

    report << "Vertex Buffer Geometry: "
           << stats.vertexBufferGeometryCount << "\n";

    report << "Missing Vertex Data: "
           << stats.missingVertexDataCount << "\n";

    report << "Missing Triangle Data: "
           << stats.missingTriangleDataCount << "\n\n";

    report << "FRAME HIERARCHY\n";
    report << "----------------------------------------\n";

    report << "Frame Count: " << frames.size() << "\n";
    report << "Frame Limit Reached: "
           << (frameTruncated ? "YES" : "NO") << "\n";

    report << "Depth Limit Reached: "
           << (depthLimited ? "YES" : "NO") << "\n\n";

    std::unordered_map<RwFrame*, uint32_t> frameIndices;

    for (const FrameInfo& info : frames) {
        frameIndices[info.frame] = info.index;
    }

    std::unordered_map<
        RwFrame*,
        std::vector<const AtomicGeometryInfo*>
    > atomicsByFrame;

    for (const AtomicGeometryInfo& info : details) {
        if (info.frame != nullptr)
            atomicsByFrame[info.frame].push_back(&info);
    }

    for (const FrameInfo& info : frames) {
        report << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "FRAME #" << info.index
               << ": " << info.name << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "Depth: " << info.depth << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "Parent: ";

        const auto parentIt = frameIndices.find(info.parent);

        if (info.parent == nullptr) {
            report << "none\n";
        } else if (parentIt != frameIndices.end()) {
            report << "FRAME #" << parentIt->second << "\n";
        } else {
            report << "outside scanned hierarchy\n";
        }

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "Local Position: ";
        WriteVector(report, info.localPosition);
        report << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "World Position: ";
        WriteVector(report, info.worldPosition);
        report << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "World Right: ";
        WriteVector(report, info.right);
        report << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "World Up: ";
        WriteVector(report, info.up);
        report << "\n";

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "World At: ";
        WriteVector(report, info.at);
        report << "\n";

        const auto atomicIt = atomicsByFrame.find(info.frame);

        const size_t atomicCount =
            atomicIt != atomicsByFrame.end()
                ? atomicIt->second.size()
                : 0;

        for (uint32_t i = 0; i < info.depth; ++i)
            report << "  ";

        report << "Atomic Count: " << atomicCount << "\n";

        if (atomicIt != atomicsByFrame.end()) {
            for (const AtomicGeometryInfo* atomicInfo :
                 atomicIt->second) {

                if (atomicInfo == nullptr)
                    continue;

                for (uint32_t i = 0; i <= info.depth; ++i)
                    report << "  ";

                report << "Atomic #" << atomicInfo->atomicIndex
                       << " | Geometry #"
                       << atomicInfo->geometryIndex
                       << " | Vertices: "
                       << atomicInfo->vertexCount
                       << " | Triangles: "
                       << atomicInfo->triangleCount
                       << "\n";
            }
        }
    }

    report << "\n----------------------------------------\n";
    report << "ATOMICS WITHOUT A SCANNED FRAME\n";
    report << "----------------------------------------\n";

    uint32_t unlinkedCount = 0;

    for (const AtomicGeometryInfo& info : details) {
        if (info.frame != nullptr &&
            frameIndices.find(info.frame) != frameIndices.end()) {
            continue;
        }

        ++unlinkedCount;

        report << "Atomic #" << info.atomicIndex
               << " | Geometry #" << info.geometryIndex
               << " | Frame: ";

        if (info.frame == nullptr)
            report << "null\n";
        else
            report << "outside scanned hierarchy\n";
    }

    report << "Unlinked Atomic Count: "
           << unlinkedCount << "\n";

    report << "\n----------------------------------------\n";
    report << "PER-ATOMIC LOCAL GEOMETRY BOUNDS\n";
    report << "Coordinates are geometry-local, not vehicle/world coordinates.\n";
    report << "----------------------------------------\n";

    for (const AtomicGeometryInfo& info : details) {
        report << "\nAtomic #" << info.atomicIndex
               << " | Geometry #" << info.geometryIndex << "\n";

        report << "Vertices: " << info.vertexCount
               << " | Triangles: " << info.triangleCount << "\n";

        if (!info.hasCpuVertices ||
            info.validVertexCount == 0) {

            report << "Bounds: unavailable "
                   << "(CPU vertex positions unavailable)\n";

            continue;
        }

        report << "Valid Vertices: "
               << info.validVertexCount << "\n";

        report << "Min: X=" << info.minX
               << " Y=" << info.minY
               << " Z=" << info.minZ << "\n";

        report << "Max: X=" << info.maxX
               << " Y=" << info.maxY
               << " Z=" << info.maxZ << "\n";

        report << "Size: X=" << (info.maxX - info.minX)
               << " Y=" << (info.maxY - info.minY)
               << " Z=" << (info.maxZ - info.minZ) << "\n";
    }

    report << "\n========================================\n";
    report << "END OF DIAGNOSTIC\n";
    report << "========================================\n";

    std::string result = report.str();

    // Pastikan laporan tidak melewati batas ProfileStorage (256 KB).
    if (result.size() > MAX_DIAGNOSTIC_OUTPUT) {
        result.resize(MAX_DIAGNOSTIC_OUTPUT);

        result +=
            "\n\n[DIAGNOSTIC TRUNCATED: OUTPUT SIZE LIMIT REACHED]\n";
    }

    return result;
}

static void SaveDiagnosticReport(
    int modelId,
    const GeometryScanStats& stats,
    const std::vector<AtomicGeometryInfo>& details,
    const std::vector<FrameInfo>& frames,
    bool frameTruncated,
    bool depthLimited
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

    const std::string report = BuildDiagnosticReport(
        modelId,
        stats,
        details,
        frames,
        frameTruncated,
        depthLimited
    );

    if (ProfileStorage::SaveDiagnosticText(fileName, report)) {
        g_lastSavedModelId = modelId;

        logger->Info(
            "[VLS] Geometry diagnostic saved: %s",
            fileName
        );

        logger->Info(
            "[VLS] Frame hierarchy count: %u",
            static_cast<unsigned int>(frames.size())
        );
    } else {
        logger->Error(
            "[VLS] Failed to save geometry diagnostic: %s",
            fileName
        );
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
        logger->Info(
            "[VLS] Geometry scan deferred: vehicle clump unavailable."
        );

        return false;
    }

    std::vector<AtomicGeometryInfo> details;
    std::vector<FrameInfo> frames;

    ScanContext atomicContext;

    atomicContext.stats = &stats;
    atomicContext.details = &details;

    RpClumpForAllAtomics(
        clump,
        ScanAtomic,
        &atomicContext
    );

    // Baca root frame kendaraan dari clump.
    RwFrame* rootFrame = RpClumpGetFrame(clump);

    FrameScanContext frameContext;
    frameContext.frames = &frames;

    if (rootFrame != nullptr) {
        ScanFrameBranch(
            rootFrame,
            0,
            frameContext
        );
    } else {
        logger->Info(
            "[VLS] Frame hierarchy unavailable: clump root frame is null."
        );
    }

    logger->Info("[VLS] Vehicle Geometry Scan");
    logger->Info("[VLS] Model ID: %d", modelId);

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
        "[VLS] Frame Count: %u",
        static_cast<unsigned int>(frames.size())
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

    if (frameContext.truncated) {
        logger->Info(
            "[VLS] Frame scan stopped: maximum frame count reached."
        );
    }

    if (frameContext.depthLimited) {
        logger->Info(
            "[VLS] Frame scan stopped: maximum hierarchy depth reached."
        );
    }

    SaveDiagnosticReport(
        modelId,
        stats,
        details,
        frames,
        frameContext.truncated,
        frameContext.depthLimited
    );

    return stats.atomicCount > 0;
}

} // namespace VLS
