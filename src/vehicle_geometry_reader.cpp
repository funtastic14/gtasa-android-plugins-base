
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

struct GlassPart {
 uint32_t triangles=0, vertices=0;
 float minX=0,minY=0,minZ=0,maxX=0,maxY=0,maxZ=0;
 float cx=0,cy=0,cz=0,nx=0,ny=0,nz=0,area=0;
 std::vector<std::pair<int16_t, uint32_t>> materialCounts;
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
};

struct FrameScanContext {
    std::vector<FrameInfo>* frames = nullptr;
    std::unordered_set<RwFrame*> visited;

    bool truncated = false;
    bool depthLimited = false;
};

static int g_lastSavedModelId = -2147483647;
static std::vector<AtomicMesh> g_cachedMeshes;

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
    const uint32_t materialCount = signedMaterialCount > 0
        ? static_cast<uint32_t>(signedMaterialCount) : 0;

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

    const uint32_t safeMaterialCount = materialCount > 1024u ? 1024u : materialCount;
    info.materials.reserve(safeMaterialCount);
    for (uint32_t i = 0; i < safeMaterialCount; ++i) {
        MeshMaterialInfo materialInfo;
        materialInfo.index = static_cast<int16_t>(i);
        RpMaterial* material = RpGeometryGetMaterial(geometry, static_cast<RwInt32>(i));
        RwTexture* texture = material != nullptr ? RpMaterialGetTexture(material) : nullptr;
        if (texture != nullptr) {
            materialInfo.textureName = ReadTextureName(RwTextureGetName(texture));
            materialInfo.maskName = ReadTextureName(RwTextureGetMaskName(texture));
        }
        info.materials.push_back(materialInfo);
    }

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
    else if (vertexCount == 0 ||
             vertexCount > 1000000u) {
        ++stats.skippedVertexLimitCount;
    }
    else if (triangleCount == 0 ||
             sourceTriangles == nullptr) {
        ++stats.skippedNoTriangleDataCount;
    }
    else {
        AtomicMesh mesh;

        mesh.atomicIndex = atomicIndex;
        mesh.geometryIndex = geometryIndex;
        mesh.frame = info.frame;
        mesh.materials = info.materials;

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
struct VehicleSpaceTransform {
    RwV3d origin{}, inverseRowX{}, inverseRowY{}, inverseRowZ{};
    bool valid = false;
};
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

    // "lataria" is the vehicle body/chassis, never a glass or lighting
    // surface. This explicit semantic exclusion takes priority over material
    // texture names and all candidate heuristics below.
    if (frameName.find("lataria") != std::string::npos) {
        return "BODY_CHASSIS";
    }

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

    if (frameGlass || textureGlass || (doorNamed && part.cz > 0.30f)) {
        if (ax > ay && ax > az) return "SIDE_GLASS_CANDIDATE";
        if (ay >= ax && ay > az) {
            if (part.cy > 0.25f) return "FRONT_GLASS_CANDIDATE";
            if (part.cy < -0.25f) return "REAR_GLASS_CANDIDATE";
        }
        return "GLASS_PANEL_UNRESOLVED";
    }

    // Names such as "lataria" and "alpha" are not semantic proof of glass.
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

    // Ganti cache setelah pemindaian selesai.
    g_cachedMeshes.swap(meshes);

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

const std::vector<AtomicMesh>&
VehicleGeometryReader::GetMeshes() {
    return g_cachedMeshes;
}

void VehicleGeometryReader::Clear() {
    std::vector<AtomicMesh>().swap(g_cachedMeshes);
}

} // namespace VLS
