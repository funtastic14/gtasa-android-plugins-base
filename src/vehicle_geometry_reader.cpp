
#include "vehicle_geometry_reader.h"

#include "profile_storage.h"

#include <mod/logger.h>

#include <plugin-sdk/game_sa/entity/Vehicle.h>
#include <plugin-sdk/renderware/RpClump.h>
#include <plugin-sdk/renderware/RpAtomic.h>
#include <plugin-sdk/renderware/RpGeometry.h>
#include <plugin-sdk/renderware/RpMaterial.h>
#include <plugin-sdk/renderware/RwTexture.h>
#include <plugin-sdk/renderware/RwFrame.h>
#include <plugin-sdk/renderware/RwMatrix.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VLS {

namespace {

static const size_t MAX_FRAME_COUNT = 4096;
static const size_t MAX_DIAGNOSTIC_OUTPUT = 240 * 1024;
static const uint32_t MAX_FRAME_DEPTH = 128;

// Conservative Native RGB admission limits for low-memory Android devices.
// These are hard safety ceilings, not targets; incremental scanning will
// further reduce the amount of work performed in any single game frame.
static const uint32_t MAX_SCAN_ATOMIC_COUNT = 128;
static const uint32_t MAX_ATOMIC_VERTEX_COUNT = 8192;
static const uint32_t MAX_ATOMIC_TRIANGLE_COUNT = 12000;
static const uint64_t MAX_SCAN_TOTAL_VERTEX_COUNT = 100000;
static const uint64_t MAX_SCAN_TOTAL_TRIANGLE_COUNT = 150000;
static const uint32_t MAX_SCAN_MATERIAL_COUNT = 128;

struct GlassPart {
 uint32_t triangles=0, vertices=0;
 float minX=0,minY=0,minZ=0,maxX=0,maxY=0,maxZ=0;
 float cx=0,cy=0,cz=0,nx=0,ny=0,nz=0,area=0;
 std::vector<std::pair<int16_t, uint32_t>> materialCounts;
  std::vector<uint32_t> triangleIndices;
};
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
    std::string frameName;
    bool hasVehicleSpaceBounds = false;
    float vehicleMinX = 0.0f, vehicleMinY = 0.0f, vehicleMinZ = 0.0f;
    float vehicleMaxX = 0.0f, vehicleMaxY = 0.0f, vehicleMaxZ = 0.0f;
    uint32_t analyzedTriangleCount = 0;
    uint32_t dominantNormalXCount = 0;
    uint32_t dominantNormalYCount = 0;
    uint32_t dominantNormalZCount = 0;
    std::vector<GlassPart> glassParts;
    std::vector<MeshMaterialInfo> materials;
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
    std::vector<AtomicMesh>* meshes = nullptr;
    uint32_t geometryIndex = 0;
    bool limitExceeded = false;
};

struct FrameScanContext {
    std::vector<FrameInfo>* frames = nullptr;
    std::unordered_set<RwFrame*> visited;

    bool truncated = false;
    bool depthLimited = false;
};

static int g_lastSavedModelId = -2147483647;
static std::vector<AtomicMesh> g_cachedMeshes;
static std::vector<SurfacePatch> g_cachedSurfacePatches;

static const uint32_t INCREMENTAL_VERTEX_BUDGET = 256;
static const uint32_t INCREMENTAL_TRIANGLE_BUDGET = 384;
static const uint32_t INCREMENTAL_ATOMIC_PROBE_BUDGET = 8;

struct VehicleSpaceTransform {
    RwV3d origin{}, inverseRowX{}, inverseRowY{}, inverseRowZ{};
    bool valid = false;
};

enum class IncrementalPhase : uint8_t {
    CopyMeshes,
    TransformVertices,
    BuildConnectivity,
    AggregateComponents,
    BuildPatches,
    Complete
};

struct ConnectivityEntry {
    uint32_t key = 0;
    uint16_t triangle = 0;
    uint16_t generation = 0;
};

struct IncrementalScanState {
    CVehicle* vehicle = nullptr;
    RpClump* clump = nullptr;
    int modelId = -1;
    IncrementalPhase phase = IncrementalPhase::CopyMeshes;
    uint32_t atomicIndex = 0;
    uint32_t activeAtomicIndex = 0;
    uint32_t geometryIndex = 0;
    uint32_t atomicCount = 0;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t vertexCursor = 0;
    uint32_t triangleCursor = 0;
    uintptr_t geometryIdentity = 0;
    bool activeAtomic = false;
    bool invalidVertices = false;
    bool failed = false;
    bool complete = false;
    GeometryScanStats stats;
    AtomicMesh currentMesh;
    std::vector<AtomicMesh> meshes;

    size_t meshIndex = 0;
    uintptr_t rootFrameIdentity = 0;
    bool analysisMeshInitialized = false;
    uint32_t transformCursor = 0;
    uint32_t analysisTriangleCursor = 0;
    uint32_t patchCursor = 0;
    VehicleSpaceTransform vehicleTransform;
    AtomicGeometryInfo currentInfo;
    std::vector<uint32_t> parent;
    std::vector<uint32_t> rootToPart;
    std::vector<ConnectivityEntry> connectivityTable;
    uint16_t connectivityGeneration = 0;
};

static IncrementalScanState g_incrementalScan;

struct AtomicAtIndexContext {
    uint32_t target = 0;
    uint32_t current = 0;
    RpAtomic* found = nullptr;
};

static RpAtomic* FindAtomicAtIndex(RpAtomic* atomic, void* data) {
    if (atomic == nullptr || data == nullptr)
        return atomic;
    AtomicAtIndexContext* context =
        static_cast<AtomicAtIndexContext*>(data);
    if (context->current == context->target) {
        context->found = atomic;
        return nullptr;
    }
    ++context->current;
    return atomic;
}

static RpAtomic* GetAtomicAtIndex(RpClump* clump, uint32_t index) {
    if (clump == nullptr)
        return nullptr;
    AtomicAtIndexContext context;
    context.target = index;
    RpClumpForAllAtomics(clump, FindAtomicAtIndex, &context);
    return context.found;
}

static void ResetIncrementalScanState() {
    g_incrementalScan = IncrementalScanState{};
}

static std::string ReadTextureName(const char* name) {
    if (name == nullptr)
        return std::string();
    size_t length = 0;
    while (length < 32 && name[length] != '\0')
        ++length;
    return std::string(name, length);
}

static bool IsFiniteVertex(const RwV3d& vertex) {
    return std::isfinite(vertex.x) &&
           std::isfinite(vertex.y) &&
           std::isfinite(vertex.z);
}

static RpAtomic* ScanAtomic(RpAtomic* atomic, void* data) {
    if (atomic == nullptr || data == nullptr)
        return atomic;

    ScanContext* context = static_cast<ScanContext*>(data);

    if (context->stats == nullptr ||
        context->details == nullptr ||
        context->meshes == nullptr) {
        return atomic;
    }

    GeometryScanStats& stats = *context->stats;

    if (context->limitExceeded)
        return nullptr;

    if (stats.atomicCount >= MAX_SCAN_ATOMIC_COUNT) {
        context->limitExceeded = true;
        return nullptr;
    }

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
    const RwInt32 signedMaterialCount = RpGeometryGetNumMaterials(geometry);
    const uint32_t materialCount =
        (signedMaterialCount > 0 &&
         signedMaterialCount <= static_cast<RwInt32>(MAX_SCAN_MATERIAL_COUNT) &&
         geometry->matList.materials != nullptr)
            ? static_cast<uint32_t>(signedMaterialCount) : 0;

    // RenderWare geometry counts are signed runtime data. Reject implausible
    // values before they can drive pointer arithmetic or large allocations.
    const bool saneVertexCount =
        vertexCount > 0 && vertexCount <= MAX_ATOMIC_VERTEX_COUNT;
    const bool saneTriangleCount =
        triangleCount > 0 && triangleCount <= MAX_ATOMIC_TRIANGLE_COUNT;

    if (!saneVertexCount || !saneTriangleCount ||
        stats.totalVertices + vertexCount > MAX_SCAN_TOTAL_VERTEX_COUNT ||
        stats.totalTriangles + triangleCount > MAX_SCAN_TOTAL_TRIANGLE_COUNT) {
        context->limitExceeded = true;
        return nullptr;
    }

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

    const uint32_t safeMaterialCount = materialCount;
    info.materials.reserve(safeMaterialCount);
    for (uint32_t i = 0; i < safeMaterialCount; ++i) {
        MeshMaterialInfo materialInfo;
        materialInfo.index = static_cast<int16_t>(i);
        RpMaterial* material = geometry->matList.materials[i];
        RwTexture* texture = material != nullptr ? RpMaterialGetTexture(material) : nullptr;
        if (texture != nullptr) {
            materialInfo.textureName = ReadTextureName(RwTextureGetName(texture));
            materialInfo.maskName = ReadTextureName(RwTextureGetMaskName(texture));
            materialInfo.raster = RwTextureGetRaster(texture);
        }
        info.materials.push_back(materialInfo);
    }

    const RwV3d* vertices = nullptr;

    if (hasPositions &&
        !isNative &&
        !hasNativeInstance &&
        saneVertexCount &&
        geometry->numMorphTargets > 0 &&
        geometry->numMorphTargets <= 16 &&
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

    RpTriangle* sourceTriangles =
        RpGeometryGetTriangles(geometry);

    if (triangleCount > 0 && sourceTriangles == nullptr)
        ++stats.missingTriangleDataCount;

    // -----------------------------------------------------
    // Copy CPU geometry into the active vehicle mesh cache.
    // -----------------------------------------------------

    if (!info.hasCpuVertices || vertices == nullptr) {
        ++stats.skippedNoCpuVertexCount;
    }
    else if (!saneVertexCount) {
        ++stats.skippedVertexLimitCount;
    }
    else if (!saneTriangleCount ||
             sourceTriangles == nullptr) {
        ++stats.skippedNoTriangleDataCount;
    }
    else {
        AtomicMesh mesh;

        mesh.atomicIndex = atomicIndex;
        mesh.geometryIndex = geometryIndex;
        mesh.frame = info.frame;
        mesh.materials = info.materials;
        const bool hasUv0 =
            geometry->numTexCoordSets > 0 &&
            geometry->numTexCoordSets <= rwMAXTEXTURECOORDS &&
            geometry->texCoords[0] != nullptr;
        mesh.hasTextureCoordinates = hasUv0;

        mesh.vertices.reserve(vertexCount);

        bool validMeshVertices = true;

        for (uint32_t i = 0; i < vertexCount; ++i) {
            const RwV3d& vertex = vertices[i];

            if (!IsFiniteVertex(vertex)) {
                validMeshVertices = false;
                break;
            }

            GeometryVertex storedVertex;

            storedVertex.x = vertex.x;
            storedVertex.y = vertex.y;
            storedVertex.z = vertex.z;

            // Preserve the first UV set without modifying source geometry.
            // Some native/mobile geometries do not expose CPU UV data.
            if (hasUv0) {
                storedVertex.u = geometry->texCoords[0][i].u;
                storedVertex.v = geometry->texCoords[0][i].v;
            }

            mesh.vertices.push_back(storedVertex);
        }

        if (!validMeshVertices ||
            mesh.vertices.size() != vertexCount) {
            ++stats.skippedInvalidVertexCount;
        }
        else {
            mesh.triangles.reserve(triangleCount);

            for (uint32_t i = 0; i < triangleCount; ++i) {
                const RpTriangle& triangle =
                    sourceTriangles[i];

                const uint16_t a = triangle.vertIndex[0];
                const uint16_t b = triangle.vertIndex[1];
                const uint16_t c = triangle.vertIndex[2];

                if (a >= vertexCount ||
                    b >= vertexCount ||
                    c >= vertexCount) {
                    ++stats.invalidTriangleIndexCount;
                    continue;
                }

                GeometryTriangle storedTriangle;

                storedTriangle.a = a;
                storedTriangle.b = b;
                storedTriangle.c = c;
                storedTriangle.materialIndex = triangle.matIndex;

                mesh.triangles.push_back(storedTriangle);
            }

            if (mesh.vertices.empty() ||
                mesh.triangles.empty()) {
                ++stats.skippedNoValidTriangleCount;
            }
            else {
                // CACHE STATISTICS FIX:
                // Hitung hanya sekali, saat mesh berhasil disalin.
                ++stats.cachedMeshCount;

                stats.cachedVertexCount +=
                    static_cast<uint64_t>(mesh.vertices.size());

                stats.cachedTriangleCount +=
                    static_cast<uint64_t>(mesh.triangles.size());

                context->meshes->push_back(
                    std::move(mesh)
                );
            }
        }
    }

    context->details->push_back(info);

    return atomic;
}

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

        if (!context.visited.insert(frame).second)
            break;

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

        const RwMatrix& modelling = frame->modelling;
        const RwMatrix& ltm = frame->ltm;

        info.localPosition = modelling.pos;
        info.worldPosition = ltm.pos;

        info.right = ltm.right;
        info.up = ltm.up;
        info.at = ltm.at;

        context.frames->push_back(info);

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

static RwV3d CrossProduct(const RwV3d& a, const RwV3d& b) {
    return RwV3d{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
static float RwV3dDotProduct(const RwV3d& a, const RwV3d& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
static RwV3d TransformPointByMatrix(const RwMatrix& m, const RwV3d& p) {
    return RwV3d{
        m.right.x*p.x + m.up.x*p.y + m.at.x*p.z + m.pos.x,
        m.right.y*p.x + m.up.y*p.y + m.at.y*p.z + m.pos.y,
        m.right.z*p.x + m.up.z*p.y + m.at.z*p.z + m.pos.z
    };
}
static VehicleSpaceTransform BuildVehicleSpaceTransform(const RwMatrix& m) {
    VehicleSpaceTransform t;
    t.origin = m.pos;
    const RwV3d uxat = CrossProduct(m.up, m.at);
    const RwV3d atxr = CrossProduct(m.at, m.right);
    const RwV3d rxu = CrossProduct(m.right, m.up);
    const float det = RwV3dDotProduct(m.right, uxat);
    if (!std::isfinite(det) || std::fabs(det) < 0.000001f) return t;
    const float inv = 1.0f / det;
    t.inverseRowX = RwV3d{uxat.x*inv, uxat.y*inv, uxat.z*inv};
    t.inverseRowY = RwV3d{atxr.x*inv, atxr.y*inv, atxr.z*inv};
    t.inverseRowZ = RwV3d{rxu.x*inv, rxu.y*inv, rxu.z*inv};
    t.valid = true;
    return t;
}
static bool TransformMeshVertexToVehicle(
    const RwMatrix& frameMatrix,
    const VehicleSpaceTransform& t,
    const GeometryVertex& source,
    RwV3d& result
) {
    if (!t.valid) return false;
    const RwV3d local{source.x, source.y, source.z};
    const RwV3d world = TransformPointByMatrix(frameMatrix, local);
    const RwV3d delta{
        world.x-t.origin.x, world.y-t.origin.y, world.z-t.origin.z
    };
    result.x = RwV3dDotProduct(delta, t.inverseRowX);
    result.y = RwV3dDotProduct(delta, t.inverseRowY);
    result.z = RwV3dDotProduct(delta, t.inverseRowZ);
    return IsFiniteVertex(result);
}
static void GlassParts(const AtomicMesh& mesh,const std::vector<RwV3d>& p,std::vector<GlassPart>& out) {
 size_t n=mesh.triangles.size(); if(!n||n>100000)return;
 std::vector<uint32_t> parent(n); for(uint32_t i=0;i<n;++i)parent[i]=i;
 auto root=[&parent](uint32_t x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
 // Keep triangles with different materials in separate surface groups.
 // A single atomic can contain body panels and glass that share vertices.
 std::vector<std::unordered_map<int16_t, int32_t>> firstByVertexMaterial(mesh.vertices.size());
 for(uint32_t i=0;i<n;++i){const GeometryTriangle&t=mesh.triangles[i];uint16_t v[3]={t.a,t.b,t.c};
  for(int k=0;k<3;++k){if(v[k]>=firstByVertexMaterial.size())continue;auto& byMaterial=firstByVertexMaterial[v[k]];auto it=byMaterial.find(t.materialIndex);if(it!=byMaterial.end()){uint32_t a=root(i),b=root(uint32_t(it->second));if(a!=b)parent[b]=a;}else byMaterial.emplace(t.materialIndex,int32_t(i));}}
 std::unordered_map<uint32_t,size_t> ids; std::vector<std::unordered_set<uint16_t>> used;
 std::vector<std::unordered_map<int16_t,uint32_t>> materialCounts;
 for(uint32_t i=0;i<n;++i){uint32_t r=root(i);auto it=ids.find(r);if(it==ids.end()){GlassPart c;float h=std::numeric_limits<float>::max();c.minX=c.minY=c.minZ=h;c.maxX=c.maxY=c.maxZ=-h;size_t k=out.size();out.push_back(c);used.emplace_back();materialCounts.emplace_back();ids[r]=k;it=ids.find(r);}
  GlassPart&c=out[it->second];const GeometryTriangle&t=mesh.triangles[i];if(t.a>=p.size()||t.b>=p.size()||t.c>=p.size())continue;
  const RwV3d&a=p[t.a];const RwV3d&b=p[t.b];const RwV3d&d=p[t.c];RwV3d ab{b.x-a.x,b.y-a.y,b.z-a.z},ac{d.x-a.x,d.y-a.y,d.z-a.z};RwV3d q=CrossProduct(ab,ac);float l=std::sqrt(RwV3dDotProduct(q,q));if(!std::isfinite(l)||l<1e-5f)continue;
  float ar=l*.5f;++c.triangles;c.area+=ar;c.cx+=(a.x+b.x+d.x)*ar/3;c.cy+=(a.y+b.y+d.y)*ar/3;c.cz+=(a.z+b.z+d.z)*ar/3;c.nx+=q.x;c.ny+=q.y;c.nz+=q.z;
  const RwV3d*v[3]={&a,&b,&d};for(int k=0;k<3;++k){c.minX=std::min(c.minX,v[k]->x);c.maxX=std::max(c.maxX,v[k]->x);c.minY=std::min(c.minY,v[k]->y);c.maxY=std::max(c.maxY,v[k]->y);c.minZ=std::min(c.minZ,v[k]->z);c.maxZ=std::max(c.maxZ,v[k]->z);}
  used[it->second].insert(t.a);used[it->second].insert(t.b);used[it->second].insert(t.c);
  // Preserve source triangle indices for windshield surface projection.
  c.triangleIndices.push_back(i);
  ++materialCounts[it->second][t.materialIndex];
 }
 for(size_t i=0;i<out.size();++i){GlassPart&c=out[i];c.vertices=uint32_t(used[i].size());for(const auto& entry:materialCounts[i])c.materialCounts.push_back(entry);std::sort(c.materialCounts.begin(),c.materialCounts.end(),[](const auto&a,const auto&b){return a.first<b.first;});if(c.area>1e-6f){c.cx/=c.area;c.cy/=c.area;c.cz/=c.area;float l=std::sqrt(c.nx*c.nx+c.ny*c.ny+c.nz*c.nz);if(l>1e-6f){c.nx/=l;c.ny/=l;c.nz/=l;}}}
}

static void AnalyzeVehicleSpaceMeshes(
    RwFrame* rootFrame,
    std::vector<AtomicGeometryInfo>& details,
    const std::vector<FrameInfo>& frames,
    std::vector<AtomicMesh>& meshes
) {
    if (rootFrame == nullptr) return;
    std::unordered_map<RwFrame*, std::string> names;
    for (const FrameInfo& f : frames)
        if (f.frame != nullptr) names[f.frame] = f.name;
    std::unordered_map<uint32_t, AtomicMesh*> byAtomic;
    for (AtomicMesh& m : meshes) byAtomic[m.atomicIndex] = &m;
    const VehicleSpaceTransform vehicleTransform =
        BuildVehicleSpaceTransform(rootFrame->ltm);
    if (!vehicleTransform.valid) return;

    for (AtomicGeometryInfo& info : details) {
        const auto nameIt = names.find(info.frame);
        info.frameName = nameIt != names.end() ? nameIt->second : "<unlinked>";
        const auto meshIt = byAtomic.find(info.atomicIndex);
        if (meshIt == byAtomic.end() || info.frame == nullptr) continue;

        AtomicMesh& mesh = *meshIt->second;
        const RwMatrix& frameMatrix = info.frame->ltm;
        const float high = std::numeric_limits<float>::max();
        info.vehicleMinX = info.vehicleMinY = info.vehicleMinZ = high;
        info.vehicleMaxX = info.vehicleMaxY = info.vehicleMaxZ = -high;

        std::vector<RwV3d> points;
        points.reserve(mesh.vertices.size());
        mesh.vehicleSpaceVertices.clear();
        mesh.vehicleSpaceVertices.reserve(mesh.vertices.size());
        bool valid = true;
        for (const GeometryVertex& v : mesh.vertices) {
            RwV3d p{};
            if (!TransformMeshVertexToVehicle(frameMatrix, vehicleTransform, v, p)) {
                valid = false;
                break;
            }
            points.push_back(p);
            GeometryVertex vehicleVertex;
            vehicleVertex.x = p.x;
            vehicleVertex.y = p.y;
            vehicleVertex.z = p.z;
            mesh.vehicleSpaceVertices.push_back(vehicleVertex);
            if (p.x < info.vehicleMinX) info.vehicleMinX = p.x;
            if (p.y < info.vehicleMinY) info.vehicleMinY = p.y;
            if (p.z < info.vehicleMinZ) info.vehicleMinZ = p.z;
            if (p.x > info.vehicleMaxX) info.vehicleMaxX = p.x;
            if (p.y > info.vehicleMaxY) info.vehicleMaxY = p.y;
            if (p.z > info.vehicleMaxZ) info.vehicleMaxZ = p.z;
        }
        if (!valid || points.empty()) {
            mesh.vehicleSpaceVertices.clear();
            continue;
        }
        info.hasVehicleSpaceBounds = true;
        // Analyze every atomic, including frames with non-standard or missing glass names.
        // The report ranks broad surface candidates; it does not assert that they are glass.
        GlassParts(mesh, points, info.glassParts);

        for (const GeometryTriangle& tri : mesh.triangles) {
            if (tri.a >= points.size() || tri.b >= points.size() ||
                tri.c >= points.size()) continue;
            const RwV3d& a = points[tri.a];
            const RwV3d& b = points[tri.b];
            const RwV3d& c = points[tri.c];
            const RwV3d ab{b.x-a.x, b.y-a.y, b.z-a.z};
            const RwV3d ac{c.x-a.x, c.y-a.y, c.z-a.z};
            const RwV3d n = CrossProduct(ab, ac);
            const float len2 = RwV3dDotProduct(n, n);
            if (!std::isfinite(len2) || len2 < 0.00000001f) continue;
            const float x=std::fabs(n.x), y=std::fabs(n.y), z=std::fabs(n.z);
            if (x >= y && x >= z) ++info.dominantNormalXCount;
            else if (y >= x && y >= z) ++info.dominantNormalYCount;
            else ++info.dominantNormalZCount;
            ++info.analyzedTriangleCount;
        }
    }
}

static std::string SurfaceRoleHint(const AtomicGeometryInfo& info, const GlassPart& part) {
    std::string frameName = info.frameName;
    std::transform(frameName.begin(), frameName.end(), frameName.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // "lataria" names the body/chassis frame. It must not be classified
    // from its frame name, but material-separated glass/lamp parts attached
    // to this frame must remain eligible for classification.
    const bool bodyFrame = frameName.find("lataria") != std::string::npos;

    // Surface groups are material-separated. Inspect the material actually
    // assigned to this group instead of relying on the atomic frame name alone.
    std::string textureName;
    std::string maskName;
    if (!part.materialCounts.empty()) {
        const int16_t materialIndex = part.materialCounts.front().first;
        const auto materialIt = std::find_if(
            info.materials.begin(), info.materials.end(),
            [materialIndex](const MeshMaterialInfo& item) {
                return item.index == materialIndex;
            }
        );
        if (materialIt != info.materials.end()) {
            textureName = materialIt->textureName;
            maskName = materialIt->maskName;
        }
    }
    std::transform(textureName.begin(), textureName.end(), textureName.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(maskName.begin(), maskName.end(), maskName.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const auto contains = [](const std::string& value, const char* token) {
        return value.find(token) != std::string::npos;
    };
    const bool explicitLamp =
        contains(frameName, "lamp") || contains(frameName, "light") ||
        contains(frameName, "headlight") || contains(frameName, "taillight") ||
        contains(frameName, "indicator") || contains(frameName, "signal") ||
        contains(textureName, "vehiclelight") || contains(textureName, "headlight") ||
        contains(textureName, "taillight") || contains(textureName, "indicator") ||
        contains(maskName, "vehiclelight") || contains(maskName, "headlight") ||
        contains(maskName, "taillight");
    if (explicitLamp) return "LIGHTING_CANDIDATE";

    // Generic frame/material names are common in vehicle mods. Recognize
    // only compact front/rear-facing surface islands near the atomic ends.
    // This avoids turning broad body panels, bumpers or grilles into emitters.
    const float vehicleWidth = info.hasVehicleSpaceBounds
        ? info.vehicleMaxX - info.vehicleMinX : 0.0f;
    const float vehicleLength = info.hasVehicleSpaceBounds
        ? info.vehicleMaxY - info.vehicleMinY : 0.0f;
    const float patchWidth = part.maxX - part.minX;
    const float patchHeight = part.maxZ - part.minZ;
    const float patchDepth = part.maxY - part.minY;
    const float edgeMargin = std::max(0.35f, vehicleLength * 0.12f);
    const bool frontEdge = info.hasVehicleSpaceBounds &&
        part.cy >= info.vehicleMaxY - edgeMargin;
    const bool rearEdge = info.hasVehicleSpaceBounds &&
        part.cy <= info.vehicleMinY + edgeMargin;
    const bool frontRearFacing =
        ay >= ax * 1.20f && ay >= az * 1.20f;
    const bool compactLampIsland =
        vehicleWidth > 0.5f && vehicleLength > 1.0f &&
        patchWidth >= 0.04f &&
        patchWidth <= std::min(1.80f, vehicleWidth * 0.78f) &&
        patchHeight >= 0.025f && patchHeight <= 0.85f &&
        patchDepth <= 0.35f &&
        part.area >= 0.003f && part.area <= 2.25f;
    if (frontRearFacing && compactLampIsland && (frontEdge || rearEdge))
        return "LIGHTING_CANDIDATE";

    const bool frameGlass =
        contains(frameName, "glass") || contains(frameName, "window") ||
        contains(frameName, "windscreen") || contains(frameName, "windshield") ||
        contains(frameName, "screen") || contains(frameName, "kaca");
    const bool textureGlass =
        contains(textureName, "glass") || contains(textureName, "window") ||
        contains(textureName, "windscreen") || contains(textureName, "windshield") ||
        contains(textureName, "kaca") ||
        contains(maskName, "glass") || contains(maskName, "window") ||
        contains(maskName, "windscreen") || contains(maskName, "windshield") ||
        contains(maskName, "kaca");
    const bool doorNamed =
        contains(frameName, "door") || contains(frameName, "pintu");

    const float ax = std::fabs(part.nx);
    const float ay = std::fabs(part.ny);
    const float az = std::fabs(part.nz);

    if ((!bodyFrame && frameGlass) || textureGlass || (!bodyFrame && doorNamed && part.cz > 0.30f)) {
        if (ax > ay && ax > az) return "SIDE_GLASS_CANDIDATE";
        if (ay >= ax && ay > az) {
            if (part.cy > 0.25f) return "FRONT_GLASS_CANDIDATE";
            if (part.cy < -0.25f) return "REAR_GLASS_CANDIDATE";
        }
        return "GLASS_PANEL_UNRESOLVED";
    }

    // The frame is chassis/body unless material evidence identifies an
    // attached glass or lighting component.
    if (bodyFrame) return "BODY_CHASSIS";
    return "UNCLASSIFIED_SURFACE";
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

    report << "MESH CACHE STATISTICS\n";
    report << "----------------------------------------\n";

    report << "Cached Atomic Meshes: "
           << stats.cachedMeshCount << "\n";

    report << "Cached Vertices: "
           << static_cast<unsigned long long>(
                  stats.cachedVertexCount
              ) << "\n";

    report << "Cached Triangles: "
           << static_cast<unsigned long long>(
                  stats.cachedTriangleCount
              ) << "\n";

    report << "Skipped - No CPU Vertex Data: "
           << stats.skippedNoCpuVertexCount << "\n";

    report << "Skipped - Vertex Count Limit: "
           << stats.skippedVertexLimitCount << "\n";

    report << "Skipped - No Triangle Data: "
           << stats.skippedNoTriangleDataCount << "\n";

    report << "Skipped - Invalid Vertex Data: "
           << stats.skippedInvalidVertexCount << "\n";

    report << "Skipped - No Valid Triangles: "
           << stats.skippedNoValidTriangleCount << "\n";

    report << "Invalid Triangle Indices: "
           << static_cast<unsigned long long>(
                  stats.invalidTriangleIndexCount
              ) << "\n\n";

    report << "FRAME HIERARCHY\n";
    report << "----------------------------------------\n";

    report << "Frame Count: " << frames.size() << "\n";
    report << "Frame Limit Reached: "
           << (frameTruncated ? "YES" : "NO") << "\n";

    report << "Depth Limit Reached: "
           << (depthLimited ? "YES" : "NO") << "\n\n";

    std::unordered_map<RwFrame*, uint32_t> frameIndices;

    for (const FrameInfo& info : frames)
        frameIndices[info.frame] = info.index;

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
    report << "VEHICLE-LOCAL MESH SURFACE ANALYSIS\n";
    report << "Vertex transform: geometry local -> frame LTM -> inverse vehicle root LTM.\n";
    report << "Normal axis counts classify each valid triangle by its largest absolute normal component.\n";
    report << "Measurements only; this section does not automatically identify windshields.\n";
    report << "----------------------------------------\n";
    for (const AtomicGeometryInfo& info : details) {
        if (!info.hasVehicleSpaceBounds && info.analyzedTriangleCount == 0) continue;
        report << "Atomic #" << info.atomicIndex
               << " | Geometry #" << info.geometryIndex
               << " | Frame: " << info.frameName
               << " | Vertices: " << info.vertexCount
               << " | Triangles: " << info.triangleCount << "\n";
        if (info.hasVehicleSpaceBounds) {
            report << "  Vehicle Min: X=" << info.vehicleMinX
                   << " Y=" << info.vehicleMinY << " Z=" << info.vehicleMinZ << "\n";
            report << "  Vehicle Max: X=" << info.vehicleMaxX
                   << " Y=" << info.vehicleMaxY << " Z=" << info.vehicleMaxZ << "\n";
            report << "  Vehicle Size: X=" << (info.vehicleMaxX-info.vehicleMinX)
                   << " Y=" << (info.vehicleMaxY-info.vehicleMinY)
                   << " Z=" << (info.vehicleMaxZ-info.vehicleMinZ) << "\n";
        } else report << "  Vehicle Bounds: unavailable\n";
        report << "  Analyzed Triangles: " << info.analyzedTriangleCount
               << " | Dominant Normal X/Y/Z: "
               << info.dominantNormalXCount << "/"
               << info.dominantNormalYCount << "/"
               << info.dominantNormalZCount << "\n";
    }
    report << "\n----------------------------------------\n";
    report << "\n----------------------------------------\n";
    report << "SURFACE COMPONENT CANDIDATES (ALL FRAMES)\n";
    report << "Shared-vertex connected groups from every readable atomic.\n";
    report << "Candidates are ranked by area only; geometry/name/position still require review.\n";
    report << "----------------------------------------\n";

    struct SurfaceCandidateRef {
        const AtomicGeometryInfo* atomic;
        const GlassPart* part;
    };

    std::vector<SurfaceCandidateRef> candidates;
    for (const AtomicGeometryInfo& info : details) {
        for (const GlassPart& part : info.glassParts) {
            const float width = part.maxX - part.minX;
            const float depth = part.maxY - part.minY;
            const float height = part.maxZ - part.minZ;

            // Broad screening only: retain front, side, rear and unusual glass
            // shapes without depending on frame naming conventions.
            if (part.triangles < 2 ||
                part.area < 0.02f ||
                width < 0.20f ||
                depth < 0.02f ||
                height < 0.02f)
                continue;

            candidates.push_back(SurfaceCandidateRef{&info, &part});
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const SurfaceCandidateRef& a, const SurfaceCandidateRef& b) {
            return a.part->area > b.part->area;
        }
    );

    const size_t candidateLimit = candidates.size() < 48 ? candidates.size() : 48;
    report << "Candidate Count: " << candidates.size()
           << " | Displayed: " << candidateLimit << "\n";

    for (size_t i = 0; i < candidateLimit; ++i) {
        const AtomicGeometryInfo& info = *candidates[i].atomic;
        const GlassPart& part = *candidates[i].part;

        report << "Candidate #" << i
               << " | Atomic #" << info.atomicIndex
               << " | Frame: " << info.frameName
               << " | Role Hint: " << SurfaceRoleHint(info, part)
               << " | Triangles: " << part.triangles
               << " | Vertices: " << part.vertices
               << " | Area: " << part.area << "\n"
               << "  Bounds X[" << part.minX << "," << part.maxX
               << "] Y[" << part.minY << "," << part.maxY
               << "] Z[" << part.minZ << "," << part.maxZ << "]\n"
               << "  Centroid X=" << part.cx
               << " Y=" << part.cy
               << " Z=" << part.cz
               << " | Normal X=" << part.nx
               << " Y=" << part.ny
               << " Z=" << part.nz << "\n";
        for (const auto& materialCount : part.materialCounts) {
            const int16_t materialIndex = materialCount.first;
            const auto materialIt = std::find_if(
                info.materials.begin(), info.materials.end(),
                [materialIndex](const MeshMaterialInfo& item) { return item.index == materialIndex; }
            );
            report << "  Material #" << materialIndex
                   << " | Triangles: " << materialCount.second;
            if (materialIt != info.materials.end()) {
                report << " | Texture: "
                       << (materialIt->textureName.empty() ? "<none>" : materialIt->textureName)
                       << " | Mask: "
                       << (materialIt->maskName.empty() ? "<none>" : materialIt->maskName);
            }
            report << "\n";
        }
    }

    report << "----------------------------------------\n";
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

    if (result.size() > MAX_DIAGNOSTIC_OUTPUT) {
        result.resize(MAX_DIAGNOSTIC_OUTPUT);

        result +=
            "\n\n[DIAGNOSTIC TRUNCATED: OUTPUT SIZE LIMIT REACHED]\n";
    }

    return result;
}

static SurfaceRoleCandidate ToSurfaceRole(const std::string& hint) {
    if (hint == "BODY_CHASSIS") return SurfaceRoleCandidate::BodyChassis;
    if (hint == "FRONT_GLASS_CANDIDATE") return SurfaceRoleCandidate::FrontGlass;
    if (hint == "REAR_GLASS_CANDIDATE") return SurfaceRoleCandidate::RearGlass;
    if (hint == "SIDE_GLASS_CANDIDATE") return SurfaceRoleCandidate::SideGlass;
    if (hint == "GLASS_PANEL_UNRESOLVED") return SurfaceRoleCandidate::GlassUnresolved;
    if (hint == "LIGHTING_CANDIDATE") return SurfaceRoleCandidate::Lighting;
    return SurfaceRoleCandidate::Unknown;
}

static std::vector<SurfacePatch> BuildSurfacePatches(
    const std::vector<AtomicGeometryInfo>& details,
    const std::vector<AtomicMesh>& meshes
) {
    std::vector<SurfacePatch> patches;
    std::unordered_map<uint32_t, const AtomicMesh*> meshByAtomic;
    for (const AtomicMesh& mesh : meshes) meshByAtomic[mesh.atomicIndex] = &mesh;

    for (const AtomicGeometryInfo& info : details) {
        const auto meshIt = meshByAtomic.find(info.atomicIndex);
        if (meshIt == meshByAtomic.end()) continue;
        const AtomicMesh& mesh = *meshIt->second;

        for (const GlassPart& part : info.glassParts) {
            if (part.triangleIndices.empty() || part.materialCounts.empty()) continue;
            const int16_t materialIndex = part.materialCounts.front().first;
            const auto materialIt = std::find_if(
                info.materials.begin(), info.materials.end(),
                [materialIndex](const MeshMaterialInfo& item) {
                    return item.index == materialIndex;
                }
            );

            SurfacePatch patch;
            patch.atomicIndex = info.atomicIndex;
            patch.materialIndex = materialIndex;
            patch.frameName = info.frameName;
            patch.role = ToSurfaceRole(SurfaceRoleHint(info, part));
            patch.triangleIndices = part.triangleIndices;
            patch.centroid = {part.cx, part.cy, part.cz};
            patch.normal = {part.nx, part.ny, part.nz};
            patch.minBounds = {part.minX, part.minY, part.minZ};
            patch.maxBounds = {part.maxX, part.maxY, part.maxZ};
            patch.area = part.area;
            if (materialIt != info.materials.end()) {
                patch.textureName = materialIt->textureName;
                patch.maskName = materialIt->maskName;
            }
            patches.push_back(std::move(patch));
        }
    }
    return patches;
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
    std::vector<AtomicMesh> meshes;

    ScanContext atomicContext;

    atomicContext.stats = &stats;
    atomicContext.details = &details;
    atomicContext.meshes = &meshes;

    RpClumpForAllAtomics(
        clump,
        ScanAtomic,
        &atomicContext
    );

    if (atomicContext.limitExceeded) {
        // Never publish a partial mesh cache: one oversized atomic or an
        // over-budget vehicle disables Native RGB for this vehicle safely.
        Clear();
        logger->Info(
            "[VLS] Native RGB skipped: geometry budget exceeded "
            "(atomics=%u, vertices=%llu, triangles=%llu).",
            static_cast<unsigned int>(stats.atomicCount),
            static_cast<unsigned long long>(stats.totalVertices),
            static_cast<unsigned long long>(stats.totalTriangles)
        );
        return false;
    }

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

    AnalyzeVehicleSpaceMeshes(rootFrame, details, frames, meshes);

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

    // CACHE STATISTICS FIX:
    // Jangan menjumlahkan vertex dan triangle di sini lagi.
    // ScanAtomic() sudah menghitungnya tepat saat mesh berhasil disalin.
    // Hanya sinkronkan jumlah mesh dengan ukuran vector hasil scan.
    stats.cachedMeshCount =
        static_cast<uint32_t>(meshes.size());

    SaveDiagnosticReport(
        modelId,
        stats,
        details,
        frames,
        frameContext.truncated,
        frameContext.depthLimited
    );

    // Build surface patches while diagnostic geometry metadata is still available.
    std::vector<SurfacePatch> surfacePatches = BuildSurfacePatches(details, meshes);

    // Replace caches only after the scan and patch extraction are complete.
    g_cachedMeshes.swap(meshes);
    g_cachedSurfacePatches.swap(surfacePatches);

    logger->Info(
        "[VLS] Cached CPU meshes: %u",
        static_cast<unsigned int>(g_cachedMeshes.size())
    );

    logger->Info(
        "[VLS] Cached vertices: %llu",
        static_cast<unsigned long long>(stats.cachedVertexCount)
    );

    logger->Info(
        "[VLS] Cached triangles: %llu",
        static_cast<unsigned long long>(stats.cachedTriangleCount)
    );

    logger->Info(
        "[VLS] Skipped meshes - no CPU vertices: %u",
        static_cast<unsigned int>(stats.skippedNoCpuVertexCount)
    );

    logger->Info(
        "[VLS] Skipped meshes - vertex limit: %u",
        static_cast<unsigned int>(stats.skippedVertexLimitCount)
    );

    logger->Info(
        "[VLS] Skipped meshes - no triangle data: %u",
        static_cast<unsigned int>(stats.skippedNoTriangleDataCount)
    );

    logger->Info(
        "[VLS] Skipped meshes - invalid vertices: %u",
        static_cast<unsigned int>(stats.skippedInvalidVertexCount)
    );

    logger->Info(
        "[VLS] Skipped meshes - no valid triangles: %u",
        static_cast<unsigned int>(stats.skippedNoValidTriangleCount)
    );

    logger->Info(
        "[VLS] Invalid triangle indices: %llu",
        static_cast<unsigned long long>(
            stats.invalidTriangleIndexCount
        )
    );

    return stats.atomicCount > 0;
}


bool VehicleGeometryReader::ScanIncremental(
    CVehicle* vehicle,
    int modelId,
    GeometryScanStats& stats,
    bool& complete
) {
    complete = false;
    stats = GeometryScanStats{};

    if (vehicle == nullptr || modelId < 0) {
        ResetIncrementalScanState();
        Clear();
        complete = true;
        return false;
    }

    RpClump* clump = vehicle->m_pRwClump;
    if (clump == nullptr) {
        ResetIncrementalScanState();
        Clear();
        return false;
    }

    IncrementalScanState& state = g_incrementalScan;
    if (state.vehicle != vehicle ||
        state.clump != clump ||
        state.modelId != modelId) {
        ResetIncrementalScanState();
        Clear();
        state.vehicle = vehicle;
        state.clump = clump;
        state.modelId = modelId;
        state.atomicCount = static_cast<uint32_t>(
            std::max<RwInt32>(0, RpClumpGetNumAtomics(clump))
        );
        if (state.atomicCount > MAX_SCAN_ATOMIC_COUNT) {
            state.failed = true;
            state.complete = true;
        }
    }

    if (state.complete) {
        stats = state.stats;
        complete = true;
        return !state.failed;
    }

    const auto failClosed = [&]() -> bool {
        const GeometryScanStats failedStats = state.stats;
        ResetIncrementalScanState();
        state.vehicle = vehicle;
        state.clump = clump;
        state.modelId = modelId;
        state.stats = failedStats;
        state.failed = true;
        state.complete = true;
        state.phase = IncrementalPhase::Complete;
        std::vector<AtomicMesh>().swap(g_cachedMeshes);
        std::vector<SurfacePatch>().swap(g_cachedSurfacePatches);
        logger->Info(
            "[VLS] Native RGB geometry pipeline rejected; cache remains empty."
        );
        stats = state.stats;
        complete = true;
        return false;
    };

    if (state.phase != IncrementalPhase::CopyMeshes &&
        static_cast<uint32_t>(
            std::max<RwInt32>(0, RpClumpGetNumAtomics(clump))
        ) != state.atomicCount)
        return failClosed();

    if (state.phase == IncrementalPhase::CopyMeshes) {
        uint32_t vertexWork = 0;
        uint32_t triangleWork = 0;
        uint32_t atomicProbes = 0;

        while (vertexWork < INCREMENTAL_VERTEX_BUDGET &&
               triangleWork < INCREMENTAL_TRIANGLE_BUDGET &&
               atomicProbes < INCREMENTAL_ATOMIC_PROBE_BUDGET) {
            if (!state.activeAtomic &&
                state.atomicIndex >= state.atomicCount) {
                state.complete = true;
                break;
            }

            const uint32_t lookupIndex = state.activeAtomic
                ? state.activeAtomicIndex : state.atomicIndex;
            if (static_cast<uint32_t>(
                    std::max<RwInt32>(0, RpClumpGetNumAtomics(clump))
                ) != state.atomicCount)
                return failClosed();

            RpAtomic* atomic = GetAtomicAtIndex(clump, lookupIndex);
            ++atomicProbes;
            if (atomic == nullptr)
                return failClosed();

            RpGeometry* geometry = RpAtomicGetGeometry(atomic);
            if (!state.activeAtomic) {
                ++state.stats.atomicCount;
                state.activeAtomicIndex = state.atomicIndex;
                ++state.atomicIndex;
                if (geometry == nullptr)
                    continue;

                const RwInt32 signedVertices =
                    RpGeometryGetNumVertices(geometry);
                const RwInt32 signedTriangles =
                    RpGeometryGetNumTriangles(geometry);
                const uint32_t vertices = signedVertices > 0
                    ? static_cast<uint32_t>(signedVertices) : 0u;
                const uint32_t triangles = signedTriangles > 0
                    ? static_cast<uint32_t>(signedTriangles) : 0u;

                const bool saneVertices =
                    vertices > 0 && vertices <= MAX_ATOMIC_VERTEX_COUNT;
                const bool saneTriangles =
                    triangles > 0 && triangles <= MAX_ATOMIC_TRIANGLE_COUNT;

                if (!saneVertices || !saneTriangles ||
                    state.stats.totalVertices + vertices >
                        MAX_SCAN_TOTAL_VERTEX_COUNT ||
                    state.stats.totalTriangles + triangles >
                        MAX_SCAN_TOTAL_TRIANGLE_COUNT)
                    return failClosed();

                state.stats.totalVertices += vertices;
                state.stats.totalTriangles += triangles;
                ++state.stats.geometryCount;
                const uint32_t currentGeometryIndex = state.geometryIndex++;

                const RwUInt32 flags = RpGeometryGetFlags(geometry);
                const bool cpuVertices =
                    (flags & rpGEOMETRYPOSITIONS) != 0 &&
                    (flags & (rpGEOMETRYNATIVE |
                              rpGEOMETRYNATIVEINSTANCE)) == 0 &&
                    geometry->numMorphTargets > 0 &&
                    geometry->numMorphTargets <= 16 &&
                    geometry->morphTarget != nullptr &&
                    RpMorphTargetGetVertices(
                        RpGeometryGetMorphTarget(geometry, 0)
                    ) != nullptr;

                if (!cpuVertices) {
                    ++state.stats.missingVertexDataCount;
                    ++state.stats.skippedNoCpuVertexCount;
                    continue;
                }
                if (geometry->triangles == nullptr) {
                    ++state.stats.missingTriangleDataCount;
                    ++state.stats.skippedNoTriangleDataCount;
                    continue;
                }

                state.geometryIdentity =
                    reinterpret_cast<uintptr_t>(geometry);
                state.vertexCount = vertices;
                state.triangleCount = triangles;
                state.vertexCursor = 0;
                state.triangleCursor = 0;
                state.invalidVertices = false;
                state.currentMesh = AtomicMesh{};
                state.currentMesh.atomicIndex = state.activeAtomicIndex;
                state.currentMesh.geometryIndex = currentGeometryIndex;
                state.currentMesh.geometryIdentity =
                    reinterpret_cast<uintptr_t>(geometry);
                state.currentMesh.frame = RpAtomicGetFrame(atomic);
                state.currentMesh.hasTextureCoordinates =
                    geometry->numTexCoordSets > 0 &&
                    geometry->numTexCoordSets <= rwMAXTEXTURECOORDS &&
                    geometry->texCoords[0] != nullptr;

                const RwInt32 materialCount =
                    RpGeometryGetNumMaterials(geometry);
                if (materialCount > 0 &&
                    materialCount <= static_cast<RwInt32>(MAX_SCAN_MATERIAL_COUNT) &&
                    geometry->matList.materials != nullptr) {
                    state.currentMesh.materials.reserve(
                        static_cast<size_t>(materialCount)
                    );
                    for (RwInt32 i = 0; i < materialCount; ++i) {
                        MeshMaterialInfo materialInfo;
                        materialInfo.index = static_cast<int16_t>(i);
                        RpMaterial* material = geometry->matList.materials[i];
                        RwTexture* texture = material != nullptr
                            ? RpMaterialGetTexture(material) : nullptr;
                        if (texture != nullptr) {
                            materialInfo.textureName =
                                ReadTextureName(RwTextureGetName(texture));
                            materialInfo.maskName =
                                ReadTextureName(RwTextureGetMaskName(texture));
                            materialInfo.raster = RwTextureGetRaster(texture);
                        }
                        state.currentMesh.materials.push_back(
                            std::move(materialInfo)
                        );
                    }
                }

                state.currentMesh.vertices.resize(vertices);
                state.currentMesh.triangles.reserve(triangles);
                state.activeAtomic = true;
                ++state.stats.cpuVertexGeometryCount;
            }

            // Re-resolve the atomic and geometry every frame. No RenderWare
            // geometry/vertex/triangle pointer is retained between frames.
            if (geometry == nullptr ||
                reinterpret_cast<uintptr_t>(geometry) !=
                    state.geometryIdentity ||
                RpAtomicGetFrame(atomic) != state.currentMesh.frame)
                return failClosed();

            const RpMorphTarget* morphTarget =
                RpGeometryGetMorphTarget(geometry, 0);
            const RwV3d* sourceVertices =
                morphTarget != nullptr
                    ? RpMorphTargetGetVertices(morphTarget) : nullptr;
            if (sourceVertices == nullptr)
                return failClosed();

            const bool hasUv =
                state.currentMesh.hasTextureCoordinates &&
                geometry->texCoords[0] != nullptr;
            const uint32_t vertexEnd = std::min(
                state.vertexCount,
                state.vertexCursor +
                    (INCREMENTAL_VERTEX_BUDGET - vertexWork)
            );
            for (; state.vertexCursor < vertexEnd; ++state.vertexCursor) {
                const RwV3d& source = sourceVertices[state.vertexCursor];
                if (!IsFiniteVertex(source)) {
                    state.invalidVertices = true;
                    break;
                }
                GeometryVertex& target =
                    state.currentMesh.vertices[state.vertexCursor];
                target.x = source.x;
                target.y = source.y;
                target.z = source.z;
                if (hasUv) {
                    target.u = geometry->texCoords[0][state.vertexCursor].u;
                    target.v = geometry->texCoords[0][state.vertexCursor].v;
                }
                ++vertexWork;
            }

            if (state.invalidVertices) {
                ++state.stats.skippedInvalidVertexCount;
                state.activeAtomic = false;
                state.geometryIdentity = 0;
                state.currentMesh = AtomicMesh{};
                continue;
            }
            if (state.vertexCursor < state.vertexCount)
                break;

            const RpTriangle* sourceTriangles =
                RpGeometryGetTriangles(geometry);
            if (sourceTriangles == nullptr) {
                ++state.stats.missingTriangleDataCount;
                ++state.stats.skippedNoTriangleDataCount;
                state.activeAtomic = false;
                state.geometryIdentity = 0;
                state.currentMesh = AtomicMesh{};
                continue;
            }

            const uint32_t triangleEnd = std::min(
                state.triangleCount,
                state.triangleCursor +
                    (INCREMENTAL_TRIANGLE_BUDGET - triangleWork)
            );
            for (; state.triangleCursor < triangleEnd; ++state.triangleCursor) {
                ++triangleWork;
                const RpTriangle& source = sourceTriangles[state.triangleCursor];
                const uint16_t a = source.vertIndex[0];
                const uint16_t b = source.vertIndex[1];
                const uint16_t c = source.vertIndex[2];
                if (a >= state.vertexCount ||
                    b >= state.vertexCount ||
                    c >= state.vertexCount) {
                    ++state.stats.invalidTriangleIndexCount;
                    continue;
                }
                GeometryTriangle triangle;
                triangle.a = a;
                triangle.b = b;
                triangle.c = c;
                triangle.materialIndex = source.matIndex;
                state.currentMesh.triangles.push_back(triangle);
            }

            if (state.triangleCursor < state.triangleCount)
                break;

            if (!state.currentMesh.vertices.empty() &&
                !state.currentMesh.triangles.empty()) {
                ++state.stats.cachedMeshCount;
                state.stats.cachedVertexCount +=
                    state.currentMesh.vertices.size();
                state.stats.cachedTriangleCount +=
                    state.currentMesh.triangles.size();
                state.meshes.push_back(std::move(state.currentMesh));
            } else {
                ++state.stats.skippedNoValidTriangleCount;
            }

            state.activeAtomic = false;
            state.geometryIdentity = 0;
        }

        if (state.complete && state.failed)
            return failClosed();

        if (state.complete) {
            g_cachedMeshes.swap(state.meshes);
            g_cachedSurfacePatches.clear();
            state.complete = false;
            state.phase = IncrementalPhase::TransformVertices;
            state.meshIndex = 0;
            state.analysisMeshInitialized = false;
            logger->Info(
                "[VLS] Incremental CPU mesh copy complete: atomics=%u meshes=%u vertices=%llu triangles=%llu",
                static_cast<unsigned int>(state.stats.atomicCount),
                static_cast<unsigned int>(state.stats.cachedMeshCount),
                static_cast<unsigned long long>(state.stats.cachedVertexCount),
                static_cast<unsigned long long>(state.stats.cachedTriangleCount)
            );
        }

        stats = state.stats;
        return false;
    }

    if (state.phase == IncrementalPhase::TransformVertices) {
        if (state.meshIndex >= g_cachedMeshes.size()) {
            state.phase = IncrementalPhase::Complete;
            state.complete = true;
            logger->Info(
                "[VLS] Native RGB surface analysis complete: meshes=%u patches=%u",
                static_cast<unsigned int>(g_cachedMeshes.size()),
                static_cast<unsigned int>(g_cachedSurfacePatches.size())
            );
            stats = state.stats;
            complete = true;
            return true;
        }

        AtomicMesh& mesh = g_cachedMeshes[state.meshIndex];
        RpAtomic* liveAtomic = GetAtomicAtIndex(clump, mesh.atomicIndex);
        if (liveAtomic == nullptr ||
            RpAtomicGetFrame(liveAtomic) != mesh.frame ||
            reinterpret_cast<uintptr_t>(
                RpAtomicGetGeometry(liveAtomic)
            ) != mesh.geometryIdentity)
            return failClosed();

        if (!state.analysisMeshInitialized) {
            if (mesh.frame == nullptr || mesh.vertices.empty()) {
                ++state.meshIndex;
                stats = state.stats;
                return false;
            }

            state.currentInfo = AtomicGeometryInfo{};
            state.currentInfo.atomicIndex = mesh.atomicIndex;
            state.currentInfo.geometryIndex = mesh.geometryIndex;
            state.currentInfo.frame = mesh.frame;
            state.currentInfo.vertexCount =
                static_cast<uint32_t>(mesh.vertices.size());
            state.currentInfo.triangleCount =
                static_cast<uint32_t>(mesh.triangles.size());
            state.currentInfo.hasCpuVertices = true;
            state.currentInfo.materials = mesh.materials;

            const char* frameName = GetFrameNodeName(mesh.frame);
            state.currentInfo.frameName =
                frameName != nullptr && frameName[0] != '\0'
                    ? frameName : "<unnamed>";

            mesh.vehicleSpaceVertices.resize(mesh.vertices.size());
            state.transformCursor = 0;
            state.analysisMeshInitialized = true;
        }

        RwFrame* rootFrame = RpClumpGetFrame(clump);
        if (rootFrame == nullptr)
            return failClosed();

        const uintptr_t rootIdentity =
            reinterpret_cast<uintptr_t>(rootFrame);
        if (state.rootFrameIdentity == 0)
            state.rootFrameIdentity = rootIdentity;
        else if (state.rootFrameIdentity != rootIdentity)
            return failClosed();

        state.vehicleTransform =
            BuildVehicleSpaceTransform(rootFrame->ltm);
        if (!state.vehicleTransform.valid)
            return failClosed();

        const uint32_t vertexEnd = std::min(
            static_cast<uint32_t>(mesh.vertices.size()),
            state.transformCursor + INCREMENTAL_VERTEX_BUDGET
        );
        const float high = std::numeric_limits<float>::max();
        if (state.transformCursor == 0) {
            state.currentInfo.vehicleMinX =
                state.currentInfo.vehicleMinY =
                state.currentInfo.vehicleMinZ = high;
            state.currentInfo.vehicleMaxX =
                state.currentInfo.vehicleMaxY =
                state.currentInfo.vehicleMaxZ = -high;
        }

        for (; state.transformCursor < vertexEnd; ++state.transformCursor) {
            RwV3d point{};
            if (!TransformMeshVertexToVehicle(
                    mesh.frame->ltm,
                    state.vehicleTransform,
                    mesh.vertices[state.transformCursor],
                    point))
                return failClosed();

            GeometryVertex& target =
                mesh.vehicleSpaceVertices[state.transformCursor];
            target.x = point.x;
            target.y = point.y;
            target.z = point.z;

            state.currentInfo.vehicleMinX =
                std::min(state.currentInfo.vehicleMinX, point.x);
            state.currentInfo.vehicleMinY =
                std::min(state.currentInfo.vehicleMinY, point.y);
            state.currentInfo.vehicleMinZ =
                std::min(state.currentInfo.vehicleMinZ, point.z);
            state.currentInfo.vehicleMaxX =
                std::max(state.currentInfo.vehicleMaxX, point.x);
            state.currentInfo.vehicleMaxY =
                std::max(state.currentInfo.vehicleMaxY, point.y);
            state.currentInfo.vehicleMaxZ =
                std::max(state.currentInfo.vehicleMaxZ, point.z);
        }

        if (state.transformCursor >= mesh.vertices.size()) {
            state.currentInfo.hasVehicleSpaceBounds = true;
            state.parent.resize(mesh.triangles.size());
            state.rootToPart.assign(
                mesh.triangles.size(),
                std::numeric_limits<uint32_t>::max()
            );
            if (state.connectivityTable.empty())
                state.connectivityTable.resize(65536);
            ++state.connectivityGeneration;
            if (state.connectivityGeneration == 0) {
                for (ConnectivityEntry& entry : state.connectivityTable)
                    entry = ConnectivityEntry{};
                state.connectivityGeneration = 1;
            }
            state.analysisTriangleCursor = 0;
            state.phase = IncrementalPhase::BuildConnectivity;
        }

        stats = state.stats;
        return false;
    }

    if (state.phase == IncrementalPhase::BuildConnectivity) {
        AtomicMesh& mesh = g_cachedMeshes[state.meshIndex];
        RpAtomic* liveAtomic = GetAtomicAtIndex(clump, mesh.atomicIndex);
        RwFrame* liveRootFrame = RpClumpGetFrame(clump);
        if (liveRootFrame == nullptr ||
            reinterpret_cast<uintptr_t>(liveRootFrame) !=
                state.rootFrameIdentity)
            return failClosed();
        if (liveAtomic == nullptr ||
            RpAtomicGetFrame(liveAtomic) != mesh.frame ||
            reinterpret_cast<uintptr_t>(
                RpAtomicGetGeometry(liveAtomic)
            ) != mesh.geometryIdentity)
            return failClosed();
        const uint32_t triangleCount =
            static_cast<uint32_t>(mesh.triangles.size());
        const uint32_t triangleEnd = std::min(
            triangleCount,
            state.analysisTriangleCursor + INCREMENTAL_TRIANGLE_BUDGET
        );

        const auto findRoot = [&state](uint32_t value) {
            while (state.parent[value] != value) {
                state.parent[value] =
                    state.parent[state.parent[value]];
                value = state.parent[value];
            }
            return value;
        };

        for (; state.analysisTriangleCursor < triangleEnd;
             ++state.analysisTriangleCursor) {
            const uint32_t index = state.analysisTriangleCursor;
            state.parent[index] = index;
            const GeometryTriangle& triangle = mesh.triangles[index];
            const uint16_t vertices[3] = {
                triangle.a, triangle.b, triangle.c
            };

            for (uint32_t vertex : vertices) {
                const uint32_t key =
                    ((static_cast<uint32_t>(
                        static_cast<uint16_t>(triangle.materialIndex)
                    ) << 13) | vertex) + 1u;
                uint32_t slot = (key * 2654435761u) & 65535u;
                bool inserted = false;
                for (uint32_t probe = 0; probe < 65536u; ++probe) {
                    ConnectivityEntry& entry =
                        state.connectivityTable[slot];
                    if (entry.generation != state.connectivityGeneration ||
                        entry.key == 0) {
                        entry.key = key;
                        entry.triangle = static_cast<uint16_t>(index);
                        entry.generation = state.connectivityGeneration;
                        inserted = true;
                        break;
                    }
                    if (entry.key == key) {
                        uint32_t a = findRoot(index);
                        uint32_t b = findRoot(entry.triangle);
                        if (a != b)
                            state.parent[b] = a;
                        inserted = true;
                        break;
                    }
                    slot = (slot + 1u) & 65535u;
                }
                if (!inserted)
                    return failClosed();
            }
        }

        if (state.analysisTriangleCursor >= triangleCount) {
            ++state.connectivityGeneration;
            if (state.connectivityGeneration == 0) {
                for (ConnectivityEntry& entry : state.connectivityTable)
                    entry = ConnectivityEntry{};
                state.connectivityGeneration = 1;
            }
            state.analysisTriangleCursor = 0;
            state.phase = IncrementalPhase::AggregateComponents;
        }

        stats = state.stats;
        return false;
    }

    if (state.phase == IncrementalPhase::AggregateComponents) {
        AtomicMesh& mesh = g_cachedMeshes[state.meshIndex];
        RpAtomic* liveAtomic = GetAtomicAtIndex(clump, mesh.atomicIndex);
        RwFrame* liveRootFrame = RpClumpGetFrame(clump);
        if (liveRootFrame == nullptr ||
            reinterpret_cast<uintptr_t>(liveRootFrame) !=
                state.rootFrameIdentity)
            return failClosed();
        if (liveAtomic == nullptr ||
            RpAtomicGetFrame(liveAtomic) != mesh.frame ||
            reinterpret_cast<uintptr_t>(
                RpAtomicGetGeometry(liveAtomic)
            ) != mesh.geometryIdentity)
            return failClosed();
        const uint32_t triangleCount =
            static_cast<uint32_t>(mesh.triangles.size());
        const uint32_t triangleEnd = std::min(
            triangleCount,
            state.analysisTriangleCursor + INCREMENTAL_TRIANGLE_BUDGET
        );

        const auto findRoot = [&state](uint32_t value) {
            while (state.parent[value] != value) {
                state.parent[value] =
                    state.parent[state.parent[value]];
                value = state.parent[value];
            }
            return value;
        };

        uint32_t newPartsThisFrame = 0;
        for (; state.analysisTriangleCursor < triangleEnd;
             ++state.analysisTriangleCursor) {
            const uint32_t index = state.analysisTriangleCursor;
            const GeometryTriangle& triangle = mesh.triangles[index];

            const GeometryVertex& av =
                mesh.vehicleSpaceVertices[triangle.a];
            const GeometryVertex& bv =
                mesh.vehicleSpaceVertices[triangle.b];
            const GeometryVertex& cv =
                mesh.vehicleSpaceVertices[triangle.c];
            const RwV3d a{av.x, av.y, av.z};
            const RwV3d b{bv.x, bv.y, bv.z};
            const RwV3d c{cv.x, cv.y, cv.z};
            const RwV3d ab{b.x-a.x, b.y-a.y, b.z-a.z};
            const RwV3d ac{c.x-a.x, c.y-a.y, c.z-a.z};
            const RwV3d normal = CrossProduct(ab, ac);
            const float length = std::sqrt(
                RwV3dDotProduct(normal, normal)
            );
            if (!std::isfinite(length) || length < 1e-5f)
                continue;

            const uint32_t root = findRoot(index);
            uint32_t partIndex = state.rootToPart[root];

            if (partIndex == std::numeric_limits<uint32_t>::max()) {
                if (g_cachedSurfacePatches.size() +
                    state.currentInfo.glassParts.size() >= 512u)
                    continue;
                if (newPartsThisFrame >= 8u)
                    break;

                GlassPart part;
                const float high = std::numeric_limits<float>::max();
                part.minX = part.minY = part.minZ = high;
                part.maxX = part.maxY = part.maxZ = -high;
                part.materialCounts.emplace_back(
                    triangle.materialIndex, 0u
                );
                partIndex = static_cast<uint32_t>(
                    state.currentInfo.glassParts.size()
                );
                state.rootToPart[root] = partIndex;
                state.currentInfo.glassParts.push_back(std::move(part));
                ++newPartsThisFrame;
            }

            GlassPart& part =
                state.currentInfo.glassParts[partIndex];
            ++part.triangles;
            ++part.materialCounts[0].second;
            part.triangleIndices.push_back(index);

            const float area = length * 0.5f;
            part.area += area;
            part.cx += (a.x+b.x+c.x) * area / 3.0f;
            part.cy += (a.y+b.y+c.y) * area / 3.0f;
            part.cz += (a.z+b.z+c.z) * area / 3.0f;
            part.nx += normal.x;
            part.ny += normal.y;
            part.nz += normal.z;

            const RwV3d points[3] = {a, b, c};
            const uint16_t indices[3] = {
                triangle.a, triangle.b, triangle.c
            };
            for (uint32_t k = 0; k < 3; ++k) {
                const RwV3d& point = points[k];
                part.minX = std::min(part.minX, point.x);
                part.minY = std::min(part.minY, point.y);
                part.minZ = std::min(part.minZ, point.z);
                part.maxX = std::max(part.maxX, point.x);
                part.maxY = std::max(part.maxY, point.y);
                part.maxZ = std::max(part.maxZ, point.z);

                const uint32_t key =
                    ((partIndex + 1u) << 13) | indices[k];
                const uint32_t encodedKey = key + 1u;
                uint32_t slot =
                    (encodedKey * 2654435761u) & 65535u;
                bool inserted = false;
                for (uint32_t probe = 0; probe < 65536u; ++probe) {
                    ConnectivityEntry& entry =
                        state.connectivityTable[slot];
                    if (entry.generation != state.connectivityGeneration ||
                        entry.key == 0) {
                        entry.key = encodedKey;
                        entry.triangle = static_cast<uint16_t>(index);
                        entry.generation = state.connectivityGeneration;
                        ++part.vertices;
                        inserted = true;
                        break;
                    }
                    if (entry.key == encodedKey) {
                        inserted = true;
                        break;
                    }
                    slot = (slot + 1u) & 65535u;
                }
                if (!inserted)
                    return failClosed();
            }

            const float nx = std::fabs(normal.x);
            const float ny = std::fabs(normal.y);
            const float nz = std::fabs(normal.z);
            if (nx >= ny && nx >= nz)
                ++state.currentInfo.dominantNormalXCount;
            else if (ny >= nx && ny >= nz)
                ++state.currentInfo.dominantNormalYCount;
            else
                ++state.currentInfo.dominantNormalZCount;
            ++state.currentInfo.analyzedTriangleCount;
        }

        if (state.analysisTriangleCursor >= triangleCount) {
            state.patchCursor = 0;
            state.phase = IncrementalPhase::BuildPatches;
        }

        stats = state.stats;
        return false;
    }

    if (state.phase == IncrementalPhase::BuildPatches) {
        AtomicMesh& mesh = g_cachedMeshes[state.meshIndex];
        RpAtomic* liveAtomic = GetAtomicAtIndex(clump, mesh.atomicIndex);
        RwFrame* liveRootFrame = RpClumpGetFrame(clump);
        if (liveRootFrame == nullptr ||
            reinterpret_cast<uintptr_t>(liveRootFrame) !=
                state.rootFrameIdentity)
            return failClosed();
        if (liveAtomic == nullptr ||
            RpAtomicGetFrame(liveAtomic) != mesh.frame ||
            reinterpret_cast<uintptr_t>(
                RpAtomicGetGeometry(liveAtomic)
            ) != mesh.geometryIdentity)
            return failClosed();

        const uint32_t partCount =
            static_cast<uint32_t>(state.currentInfo.glassParts.size());
        const uint32_t patchEnd = std::min(
            partCount, state.patchCursor + 8u
        );

        for (; state.patchCursor < patchEnd; ++state.patchCursor) {
            GlassPart& part =
                state.currentInfo.glassParts[state.patchCursor];
            if (part.triangleIndices.empty() ||
                part.materialCounts.empty())
                continue;

            if (g_cachedSurfacePatches.size() >= 512u)
                continue;

            if (part.area > 1e-6f) {
                part.cx /= part.area;
                part.cy /= part.area;
                part.cz /= part.area;
                const float normalLength = std::sqrt(
                    part.nx*part.nx + part.ny*part.ny + part.nz*part.nz
                );
                if (normalLength > 1e-6f) {
                    part.nx /= normalLength;
                    part.ny /= normalLength;
                    part.nz /= normalLength;
                }
            }

            const int16_t materialIndex =
                part.materialCounts.front().first;
            const auto materialIt = std::find_if(
                state.currentInfo.materials.begin(),
                state.currentInfo.materials.end(),
                [materialIndex](const MeshMaterialInfo& item) {
                    return item.index == materialIndex;
                }
            );

            SurfacePatch patch;
            patch.atomicIndex = state.currentInfo.atomicIndex;
            patch.materialIndex = materialIndex;
            patch.frameName = state.currentInfo.frameName;
            patch.role = ToSurfaceRole(
                SurfaceRoleHint(state.currentInfo, part)
            );
            patch.triangleIndices =
                std::move(part.triangleIndices);
            patch.centroid = {part.cx, part.cy, part.cz};
            patch.normal = {part.nx, part.ny, part.nz};
            patch.minBounds = {part.minX, part.minY, part.minZ};
            patch.maxBounds = {part.maxX, part.maxY, part.maxZ};
            patch.area = part.area;
            if (materialIt != state.currentInfo.materials.end()) {
                patch.textureName = materialIt->textureName;
                patch.maskName = materialIt->maskName;
            }
            g_cachedSurfacePatches.push_back(std::move(patch));
        }

        if (state.patchCursor >= partCount) {
            ++state.meshIndex;
            state.analysisMeshInitialized = false;
            state.currentInfo = AtomicGeometryInfo{};
            state.parent.clear();
            state.rootToPart.clear();
            state.phase = IncrementalPhase::TransformVertices;
        }

        stats = state.stats;
        return false;
    }

    if (state.phase == IncrementalPhase::Complete) {
        state.complete = true;
        stats = state.stats;
        complete = true;
        return !state.failed;
    }

    return failClosed();
}

const std::vector<AtomicMesh>&
VehicleGeometryReader::GetMeshes() {
    return g_cachedMeshes;
}

const std::vector<SurfacePatch>& VehicleGeometryReader::GetSurfacePatches() {
    return g_cachedSurfacePatches;
}

void VehicleGeometryReader::Clear() {
    ResetIncrementalScanState();
    std::vector<AtomicMesh>().swap(g_cachedMeshes);
    std::vector<SurfacePatch>().swap(g_cachedSurfacePatches);
}

} // namespace VLS
