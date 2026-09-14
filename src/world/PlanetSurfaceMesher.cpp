#include "world/PlanetSurfaceMesher.hpp"
#include "world/ProceduralMicro.hpp"

#include "core/Determinism.hpp"
#include "world/Block.hpp"
#include "world/BiomeCatalog.hpp"
#include "world/FaceCullBitmasks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <unordered_map>

namespace elysium {
namespace {

struct Bucket {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<std::uint8_t> colors;
    int macroQuads{};
    int microQuads{};
    int aoDarkenedCorners{};
};

Color4u shade(Color4u c, float factor) {
    c.r = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.r)*factor),0,255));
    c.g = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.g)*factor),0,255));
    c.b = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.b)*factor),0,255));
    return c;
}

class Builder {
public:
    void emit(BlockType type, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
              Vec3 desiredNormal, float light, bool micro) {
        if (!blockProperties(type).solid) return;
        const Color4u c=shade(blockProperties(type).color,light);
        emitColoredCorners(type,{c,c,c,c},p0,p1,p2,p3,desiredNormal,micro,0);
    }

    void emitAo(BlockType type, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
                Vec3 desiredNormal, float light, bool micro,
                const std::array<float,4>& ao) {
        if (!blockProperties(type).solid) return;
        std::array<Color4u,4> colors{};
        int dark=0;
        for(std::size_t i=0;i<4;++i) {
            const float a=std::clamp(ao[i],0.45f,1.0f);
            if(a<0.999f) ++dark;
            colors[i]=shade(blockProperties(type).color,light*a);
        }
        emitColoredCorners(type,colors,p0,p1,p2,p3,desiredNormal,micro,dark);
    }

    void emitColored(BlockType materialTag, Color4u c, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
                     Vec3 desiredNormal, bool micro=false) {
        emitColoredCorners(materialTag,{c,c,c,c},p0,p1,p2,p3,desiredNormal,micro,0);
    }

    void emitColoredCorners(BlockType materialTag, std::array<Color4u,4> colors,
                            Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
                            Vec3 desiredNormal, bool micro=false, int darkenedCorners=0) {
        if (!blockProperties(materialTag).solid) materialTag=BlockType::Stone;
        Vec3 n = normalize(cross(p1-p0,p2-p0));
        if (lengthSq(n)<0.5f) return;
        if (dot(n,desiredNormal) < 0.0f) {
            std::swap(p1,p3);
            std::swap(colors[1],colors[3]);
            n = normalize(cross(p1-p0,p2-p0));
        }
        const std::array<Vec3,6> verts{p0,p1,p2,p0,p2,p3};
        constexpr std::array<int,6> ci{0,1,2,0,2,3};
        auto& b = buckets_[static_cast<std::size_t>(materialTag)];
        for (std::size_t i=0;i<verts.size();++i) {
            const auto& p=verts[i];
            const auto c=colors[static_cast<std::size_t>(ci[i])];
            b.vertices.insert(b.vertices.end(),{p.x,p.y,p.z});
            b.normals.insert(b.normals.end(),{n.x,n.y,n.z});
            b.colors.insert(b.colors.end(),{c.r,c.g,c.b,c.a});
        }
        if (micro) ++b.microQuads; else ++b.macroQuads;
        b.aoDarkenedCorners += darkenedCorners;
    }

    CpuMeshData finish() {
        CpuMeshData out{};
        for (int raw=1;raw<kBlockTypeCount;++raw) {
            auto& b=buckets_[static_cast<std::size_t>(raw)];
            if (b.vertices.empty()) continue;
            MaterialRange range{};
            range.material=static_cast<BlockType>(raw);
            range.firstVertex=out.vertexCount();
            range.vertexCount=static_cast<int>(b.vertices.size()/3U);
            range.quads=b.macroQuads+b.microQuads;
            out.materialRanges.push_back(range);
            out.vertices.insert(out.vertices.end(),b.vertices.begin(),b.vertices.end());
            out.normals.insert(out.normals.end(),b.normals.begin(),b.normals.end());
            out.colors.insert(out.colors.end(),b.colors.begin(),b.colors.end());
            out.macroQuads += b.macroQuads;
            out.microQuads += b.microQuads;
            out.aoDarkenedCorners += b.aoDarkenedCorners;
        }
        out.quads=out.macroQuads+out.microQuads;
        return out;
    }
private:
    std::array<Bucket,kBlockTypeCount> buckets_{};
};

enum class CellFace : int { UPos, UNeg, VPos, VNeg, RPos, RNeg };

Vec3 avg4(Vec3 a,Vec3 b,Vec3 c,Vec3 d) { return (a+b+c+d)*0.25f; }

int generatedSurfaceAt(const PlanetSurfaceSnapshot& p, CubeFace face, int u, int v) {
    u=std::clamp(u,0,PlanetSurfaceSnapshot::FaceResolution-1);
    v=std::clamp(v,0,PlanetSurfaceSnapshot::FaceResolution-1);
    const int f=static_cast<int>(face);
    const std::size_t idx=static_cast<std::size_t>(u + PlanetSurfaceSnapshot::FaceResolution *
        (v + PlanetSurfaceSnapshot::FaceResolution * f));
    return static_cast<int>(p.generatedSurfaceRadials[idx]);
}

Color4u lerpColor(Color4u a, Color4u b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch=[&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y)-static_cast<float>(x))*t));
    };
    return Color4u{ch(a.r,b.r),ch(a.g,b.g),ch(a.b,b.b),ch(a.a,b.a)};
}

// Present-safe climate tint for orbital shells + field LOD. Diversifies Temperate
// by BiomeId / height / local slope via Color4u only (no EnTT authority).
Color4u climateColor(const PlanetSurfaceSnapshot& p, CubeFace face, int u, int v, int surface) {
    const float local=hash01(p.seed,u,v,static_cast<int>(face),0xC11A7EULL);
    const float height=static_cast<float>(surface-PlanetSurfaceSnapshot::ReferenceRadial);
    const Vec3 dir=faceGridCellDirection(face,u,v,PlanetSurfaceSnapshot::FaceResolution);
    const BiomeId biome=sampleBiome(p.seed,p.planetClass,dir);
    const int nU=generatedSurfaceAt(p,face,std::clamp(u+1,0,PlanetSurfaceSnapshot::FaceResolution-1),v);
    const int nV=generatedSurfaceAt(p,face,u,std::clamp(v+1,0,PlanetSurfaceSnapshot::FaceResolution-1));
    const float slope=static_cast<float>(std::max(std::abs(nU-surface),std::abs(nV-surface)));
    Color4u c{};
    switch (p.planetClass) {
        case PlanetClass::Temperate: {
            switch (biome) {
                case BiomeId::TemperateRiverValley:
                case BiomeId::TemperateWetlandMarsh:
                    c={48,88,62,255}; break;
                case BiomeId::TemperateScrubland:
                case BiomeId::TemperateBadlands:
                    c={122,104,70,255}; break;
                case BiomeId::TemperateHighland:
                case BiomeId::TemperateMountain:
                    c={132,134,120,255}; break;
                case BiomeId::TemperateForest:
                case BiomeId::TemperateBorealFringe:
                    c={52,102,64,255}; break;
                case BiomeId::TemperateCoastalPlains:
                    c={72,118,92,255}; break;
                case BiomeId::TemperateRollingHills:
                    c={78,120,76,255}; break;
                case BiomeId::TemperateKarst:
                    c={140,138,126,255}; break;
                default:
                    c=local>0.62f?Color4u{86,124,78,255}:Color4u{66,110,73,255}; break;
            }
            if (height < -1.5f) c=lerpColor(c,{33,82,112,255},0.72f);
            else if (height < -0.5f) c=lerpColor(c,{40,90,100,255},0.45f);
            else if (height > 5.0f) c=lerpColor(c,{142,146,132,255},0.70f);
            else if (height > 3.0f) c=lerpColor(c,{120,124,110,255},0.40f);
            if (slope >= 3.0f) c=lerpColor(c,{113,118,126,255},std::clamp(slope*0.12f,0.0f,0.55f));
            break;
        }
        case PlanetClass::Barren:
            c=local>0.72f?Color4u{157,143,128,255}:Color4u{119,112,108,255};
            if (height > 3.0f) c=lerpColor(c,{145,133,120,255},0.35f);
            if (slope >= 2.0f) c=lerpColor(c,{113,118,126,255},0.30f);
            break;
        case PlanetClass::Scorched:
            c=local>0.76f?Color4u{183,78,42,255}:Color4u{91,66,61,255};
            if (height > 4.0f) c=lerpColor(c,{113,118,126,255},0.40f);
            if (slope >= 2.0f) c=lerpColor(c,{69,67,73,255},0.35f);
            break;
        case PlanetClass::Frozen:
            c=local>0.70f?Color4u{210,224,236,255}:Color4u{168,196,220,255};
            break;
        case PlanetClass::Toxic:
            c=local>0.70f?Color4u{140,158,90,255}:Color4u{72,96,58,255};
            break;
        case PlanetClass::Irradiated:
            c=local>0.72f?Color4u{180,210,190,255}:Color4u{90,110,100,255};
            break;
        case PlanetClass::Oceanic:
            if (height < 0.0f) c={28,78,120,255};
            else c=local>0.65f?Color4u{70,140,150,255}:Color4u{66,110,73,255};
            break;
        case PlanetClass::Anomalous:
            c=local>0.68f?Color4u{150,130,170,255}:Color4u{70,58,96,255};
            break;
        case PlanetClass::Count:
            c={66,110,73,255};
            break;
    }
    const float shadeFactor=std::clamp(0.90f+height*0.018f,0.72f,1.12f);
    return shade(c,shadeFactor);
}

// Field LOD keeps block material identity readable while pulling toward climate.
Color4u fieldSurfaceColor(const PlanetSurfaceSnapshot& p, CubeFace face, int u, int v,
                          int surface, BlockType material) {
    const Color4u base=blockProperties(material).color;
    const Color4u climate=climateColor(p,face,u,v,surface);
    float pull=0.28f;
    if (material==BlockType::Grass) pull=0.42f;
    else if (material==BlockType::Dirt) pull=0.34f;
    else if (material==BlockType::Stone) pull=0.22f;
    return lerpColor(base,climate,pull);
}

SurfaceCellAddress neighborOf(const PlanetSurfaceSnapshot& p, SurfaceCellAddress a, CellFace face) {
    switch(face) {
        case CellFace::UPos: ++a.u; break;
        case CellFace::UNeg: --a.u; break;
        case CellFace::VPos: ++a.v; break;
        case CellFace::VNeg: --a.v; break;
        case CellFace::RPos: ++a.radial; break;
        case CellFace::RNeg: --a.radial; break;
    }
    if (a.radial>=0 && a.radial<PlanetSurfaceSnapshot::RadialLayers) a=p.normalize(a);
    return a;
}

float faceLight(CellFace face) {
    switch(face) {
        case CellFace::RPos: return 1.0f;
        case CellFace::RNeg: return 0.60f;
        case CellFace::UPos: return 0.80f;
        case CellFace::UNeg: return 0.76f;
        case CellFace::VPos: return 0.74f;
        case CellFace::VNeg: return 0.70f;
    }
    return 0.8f;
}

struct GreedyMaskCell {
    BlockType type{BlockType::Air};
    bool active{};
};

// Axis: 0=U, 1=V, 2=R. origin is the coordinate of bit 0 (typically -1 halo).
FaceCullColumn packSnapshotColumn(const PlanetSurfaceSnapshot& planet, CubeFace face,
                                  int u, int v, int r, int axis, int origin, int count) {
    auto addressAt = [&](int i) {
        SurfaceCellAddress a{face, u, v, r};
        const int p = origin + i;
        if (axis == 0) a.u = p;
        else if (axis == 1) a.v = p;
        else a.radial = p;
        return a;
    };
    FaceCullColumn col{};
    col.solid = packPredBits(count, [&](int i) {
        return blockProperties(planet.get(addressAt(i))).solid;
    });
    col.refined = packPredBits(count, [&](int i) {
        const auto a = addressAt(i);
        if (!planet.radialInBounds(a.radial)) return false;
        return planet.hasMicroDetail(a);
    });
    return col;
}

// Rectangle merger in face-grid space (P0-18/19). Cube-sphere quads remain
// topological rectangles before projection; material must match.
template <class Emit>
void greedyMask(std::vector<GreedyMaskCell>& mask, int width, int height, Emit&& emit) {
    for (int v = 0; v < height; ++v) {
        for (int u = 0; u < width; ++u) {
            const int start = u + width * v;
            if (!mask[static_cast<std::size_t>(start)].active) continue;
            const BlockType type = mask[static_cast<std::size_t>(start)].type;
            int runW = 1;
            while (u + runW < width) {
                const auto& c = mask[static_cast<std::size_t>(u + runW + width * v)];
                if (!c.active || c.type != type) break;
                ++runW;
            }
            int runH = 1;
            bool canGrow = true;
            while (v + runH < height && canGrow) {
                for (int du = 0; du < runW; ++du) {
                    const auto& c = mask[static_cast<std::size_t>(u + du + width * (v + runH))];
                    if (!c.active || c.type != type) { canGrow = false; break; }
                }
                if (canGrow) ++runH;
            }
            emit(u, v, runW, runH, type);
            for (int dv = 0; dv < runH; ++dv)
                for (int du = 0; du < runW; ++du)
                    mask[static_cast<std::size_t>(u + du + width * (v + dv))].active = false;
        }
    }
}

// Extent (w,h) is in face-grid UV for R*, V for U* (along v then r), U for V* (along u then r).
std::array<Vec3,4> macroFaceCornersExtent(const PlanetSurfaceSnapshot& p, SurfaceCellAddress a,
                                          CellFace face, int w, int h) {
    a=p.normalize(a);
    const int u=a.u,v=a.v,r=a.radial;
    switch(face) {
        case CellFace::RPos: return {p.boundaryPosition(a.face,u,v,r+1),p.boundaryPosition(a.face,u+w,v,r+1),p.boundaryPosition(a.face,u+w,v+h,r+1),p.boundaryPosition(a.face,u,v+h,r+1)};
        case CellFace::RNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v+h,r),p.boundaryPosition(a.face,u+w,v+h,r),p.boundaryPosition(a.face,u+w,v,r)};
        case CellFace::UPos: return {p.boundaryPosition(a.face,u+1,v,r),p.boundaryPosition(a.face,u+1,v,r+h),p.boundaryPosition(a.face,u+1,v+w,r+h),p.boundaryPosition(a.face,u+1,v+w,r)};
        case CellFace::UNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v+w,r),p.boundaryPosition(a.face,u,v+w,r+h),p.boundaryPosition(a.face,u,v,r+h)};
        case CellFace::VPos: return {p.boundaryPosition(a.face,u,v+1,r),p.boundaryPosition(a.face,u+w,v+1,r),p.boundaryPosition(a.face,u+w,v+1,r+h),p.boundaryPosition(a.face,u,v+1,r+h)};
        case CellFace::VNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v,r+h),p.boundaryPosition(a.face,u+w,v,r+h),p.boundaryPosition(a.face,u+w,v,r)};
    }
    return {};
}

std::array<Vec3,4> macroFaceCorners(const PlanetSurfaceSnapshot& p, SurfaceCellAddress a, CellFace face) {
    a=p.normalize(a);
    const int u=a.u,v=a.v,r=a.radial;
    switch(face) {
        case CellFace::RPos: return {p.boundaryPosition(a.face,u,v,r+1),p.boundaryPosition(a.face,u+1,v,r+1),p.boundaryPosition(a.face,u+1,v+1,r+1),p.boundaryPosition(a.face,u,v+1,r+1)};
        case CellFace::RNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v+1,r),p.boundaryPosition(a.face,u+1,v+1,r),p.boundaryPosition(a.face,u+1,v,r)};
        case CellFace::UPos: return {p.boundaryPosition(a.face,u+1,v,r),p.boundaryPosition(a.face,u+1,v,r+1),p.boundaryPosition(a.face,u+1,v+1,r+1),p.boundaryPosition(a.face,u+1,v+1,r)};
        case CellFace::UNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v+1,r),p.boundaryPosition(a.face,u,v+1,r+1),p.boundaryPosition(a.face,u,v,r+1)};
        case CellFace::VPos: return {p.boundaryPosition(a.face,u,v+1,r),p.boundaryPosition(a.face,u+1,v+1,r),p.boundaryPosition(a.face,u+1,v+1,r+1),p.boundaryPosition(a.face,u,v+1,r+1)};
        case CellFace::VNeg: return {p.boundaryPosition(a.face,u,v,r),p.boundaryPosition(a.face,u,v,r+1),p.boundaryPosition(a.face,u+1,v,r+1),p.boundaryPosition(a.face,u+1,v,r)};
    }
    return {};
}

std::array<Vec3,4> microFaceCorners(const PlanetSurfaceSnapshot& p, const SurfaceMicroAddress& m, CellFace face) {
    const int u=m.u,v=m.v,r=m.radial;
    switch(face) {
        case CellFace::RPos: return {p.microBoundaryPosition(m.cell,u,r+1,v),p.microBoundaryPosition(m.cell,u+1,r+1,v),p.microBoundaryPosition(m.cell,u+1,r+1,v+1),p.microBoundaryPosition(m.cell,u,r+1,v+1)};
        case CellFace::RNeg: return {p.microBoundaryPosition(m.cell,u,r,v),p.microBoundaryPosition(m.cell,u,r,v+1),p.microBoundaryPosition(m.cell,u+1,r,v+1),p.microBoundaryPosition(m.cell,u+1,r,v)};
        case CellFace::UPos: return {p.microBoundaryPosition(m.cell,u+1,r,v),p.microBoundaryPosition(m.cell,u+1,r+1,v),p.microBoundaryPosition(m.cell,u+1,r+1,v+1),p.microBoundaryPosition(m.cell,u+1,r,v+1)};
        case CellFace::UNeg: return {p.microBoundaryPosition(m.cell,u,r,v),p.microBoundaryPosition(m.cell,u,r,v+1),p.microBoundaryPosition(m.cell,u,r+1,v+1),p.microBoundaryPosition(m.cell,u,r+1,v)};
        case CellFace::VPos: return {p.microBoundaryPosition(m.cell,u,r,v+1),p.microBoundaryPosition(m.cell,u+1,r,v+1),p.microBoundaryPosition(m.cell,u+1,r+1,v+1),p.microBoundaryPosition(m.cell,u,r+1,v+1)};
        case CellFace::VNeg: return {p.microBoundaryPosition(m.cell,u,r,v),p.microBoundaryPosition(m.cell,u,r+1,v),p.microBoundaryPosition(m.cell,u+1,r+1,v),p.microBoundaryPosition(m.cell,u+1,r,v)};
    }
    return {};
}

bool outsideSolid(const PlanetSurfaceSnapshot& p, const std::array<Vec3,4>& quad, Vec3 ownerCenter) {
    const Vec3 fc=avg4(quad[0],quad[1],quad[2],quad[3]);
    const Vec3 outward=normalize(fc-ownerCenter);
    return p.solidAt(fc+outward*0.004f);
}

void emitMacroFaceExtent(Builder& b,const PlanetSurfaceSnapshot& p,SurfaceCellAddress a,
                          CellFace face,BlockType type,int w,int h) {
    const auto q=macroFaceCornersExtent(p,a,face,w,h);
    const Vec3 center=p.cellCenterPosition(a);
    const Vec3 desired=normalize(avg4(q[0],q[1],q[2],q[3])-center);
    b.emit(type,q[0],q[1],q[2],q[3],desired,faceLight(face),false);
}

void emitTiledMacroBoundary(Builder& b,const PlanetSurfaceSnapshot& p,SurfaceCellAddress a,CellFace face,BlockType type) {
    // Only used when the neighbor is refined. Tiling avoids drawing hidden
    // portions of an otherwise homogeneous one-metre face.
    const int N=MicroBrick::Resolution;
    for(int i=0;i<N;++i) for(int j=0;j<N;++j) {
        SurfaceMicroAddress m{a,0,0,0};
        switch(face) {
            case CellFace::RPos: m={a,i,N-1,j}; break;
            case CellFace::RNeg: m={a,i,0,j}; break;
            case CellFace::UPos: m={a,N-1,i,j}; break;
            case CellFace::UNeg: m={a,0,i,j}; break;
            case CellFace::VPos: m={a,i,j,N-1}; break;
            case CellFace::VNeg: m={a,i,j,0}; break;
        }
        const auto q=microFaceCorners(p,m,face);
        const Vec3 mc=p.microCellCenterPosition(m);
        if (!outsideSolid(p,q,mc)) {
            const Vec3 desired=normalize(avg4(q[0],q[1],q[2],q[3])-mc);
            b.emit(type,q[0],q[1],q[2],q[3],desired,faceLight(face),true);
        }
    }
}

// Unclamped edges so a greedy rectangle may span adjacent MicroBricks on the
// same cube face. Edges in [0, N] match the clamped PlanetSurface helper.
Vec3 snapshotMicroBoundaryUnclamped(const PlanetSurfaceSnapshot& p, SurfaceCellAddress a,
                                    int uEdge, int radialEdge, int vEdge) {
    const float N = static_cast<float>(MicroBrick::Resolution);
    const float fu = static_cast<float>(a.u) + static_cast<float>(uEdge) / N;
    const float fv = static_cast<float>(a.v) + static_cast<float>(vEdge) / N;
    const float u = fu / static_cast<float>(PlanetSurfaceSnapshot::FaceResolution) * 2.0f - 1.0f;
    const float v = fv / static_cast<float>(PlanetSurfaceSnapshot::FaceResolution) * 2.0f - 1.0f;
    const Vec3 d = faceUvToDirection(a.face, u, v);
    const float rr = static_cast<float>(a.radial) + static_cast<float>(radialEdge) / N;
    return d * (p.referenceRadius + rr - static_cast<float>(PlanetSurfaceSnapshot::ReferenceRadial));
}

std::array<Vec3,4> microFaceCornersExtent(const PlanetSurfaceSnapshot& p, const SurfaceMicroAddress& m,
                                          CellFace face, int w, int h) {
    const int u=m.u,v=m.v,r=m.radial;
    const auto& a=m.cell;
    switch(face) {
        case CellFace::RPos: return {snapshotMicroBoundaryUnclamped(p,a,u,r+1,v),snapshotMicroBoundaryUnclamped(p,a,u+w,r+1,v),snapshotMicroBoundaryUnclamped(p,a,u+w,r+1,v+h),snapshotMicroBoundaryUnclamped(p,a,u,r+1,v+h)};
        case CellFace::RNeg: return {snapshotMicroBoundaryUnclamped(p,a,u,r,v),snapshotMicroBoundaryUnclamped(p,a,u,r,v+h),snapshotMicroBoundaryUnclamped(p,a,u+w,r,v+h),snapshotMicroBoundaryUnclamped(p,a,u+w,r,v)};
        case CellFace::UPos: return {snapshotMicroBoundaryUnclamped(p,a,u+1,r,v),snapshotMicroBoundaryUnclamped(p,a,u+1,r+h,v),snapshotMicroBoundaryUnclamped(p,a,u+1,r+h,v+w),snapshotMicroBoundaryUnclamped(p,a,u+1,r,v+w)};
        case CellFace::UNeg: return {snapshotMicroBoundaryUnclamped(p,a,u,r,v),snapshotMicroBoundaryUnclamped(p,a,u,r,v+w),snapshotMicroBoundaryUnclamped(p,a,u,r+h,v+w),snapshotMicroBoundaryUnclamped(p,a,u,r+h,v)};
        case CellFace::VPos: return {snapshotMicroBoundaryUnclamped(p,a,u,r,v+1),snapshotMicroBoundaryUnclamped(p,a,u+w,r,v+1),snapshotMicroBoundaryUnclamped(p,a,u+w,r+h,v+1),snapshotMicroBoundaryUnclamped(p,a,u,r+h,v+1)};
        case CellFace::VNeg: return {snapshotMicroBoundaryUnclamped(p,a,u,r,v),snapshotMicroBoundaryUnclamped(p,a,u,r+h,v),snapshotMicroBoundaryUnclamped(p,a,u+w,r+h,v),snapshotMicroBoundaryUnclamped(p,a,u+w,r,v)};
    }
    return {};
}

bool microFaceExposed(const PlanetSurfaceSnapshot& p, const SurfaceMicroAddress& m, CellFace face) {
    const auto q=microFaceCorners(p,m,face);
    const Vec3 mc=p.microCellCenterPosition(m);
    return !outsideSolid(p,q,mc);
}

struct SnapshotRefinedBrick {
    int u{};
    int v{};
    int r{};
    std::array<BlockType, MicroBrick::CellCount> cells{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongR{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongU{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongV{};
};

BlockType snapshotBrickCell(const SnapshotRefinedBrick& b, int mu, int mr, int mv) {
    return b.cells[static_cast<std::size_t>(MicroBrick::index(mu, mr, mv))];
}

// P0-20: greedy coplanar micro faces across adjacent refined MicroBricks in one
// face chunk. Exposure is P0-21 per-brick bitmask solid-solid (halo neighbor
// still packed as the extra bit). Does not merge across chunk / cube-face seams.
void emitCrossBrickMicro(Builder& b, const PlanetSurfaceSnapshot& p,
                         CubeFace face, int u0, int v0, int u1, int v1) {
    constexpr int N = MicroBrick::Resolution;
    constexpr int wr = PlanetSurfaceSnapshot::RadialLayers;
    const int nU = u1 - u0;
    const int nV = v1 - v0;
    if (nU <= 0 || nV <= 0) return;

    std::vector<SnapshotRefinedBrick> bricks;
    std::unordered_map<int, std::size_t> indexOf;
    auto pack = [&](int lu, int lv, int lr) { return lu + nU * (lv + nV * lr); };
    for (int v = v0; v < v1; ++v)
        for (int u = u0; u < u1; ++u)
            for (int r = 0; r < wr; ++r) {
                const SurfaceCellAddress a{face, u, v, r};
                if (!p.hasMicroDetail(a)) continue;
                SnapshotRefinedBrick brick;
                brick.u = u;
                brick.v = v;
                brick.r = r;
                p.sampleMicroBrick(a, brick.cells);
                indexOf[pack(u - u0, v - v0, r)] = bricks.size();
                bricks.push_back(brick);
            }
    if (bricks.empty()) return;

    auto typeAt = [&](int u, int v, int r, int mu, int mr, int mv) -> BlockType {
        int lu = u - u0, lv = v - v0, lr = r;
        while (mu < 0) { mu += N; --lu; } while (mu >= N) { mu -= N; ++lu; }
        while (mv < 0) { mv += N; --lv; } while (mv >= N) { mv -= N; ++lv; }
        while (mr < 0) { mr += N; --lr; } while (mr >= N) { mr -= N; ++lr; }
        if (lu >= 0 && lu < nU && lv >= 0 && lv < nV && lr >= 0 && lr < wr) {
            const auto it = indexOf.find(pack(lu, lv, lr));
            if (it != indexOf.end())
                return snapshotBrickCell(bricks[it->second], mu, mr, mv);
        }
        SurfaceCellAddress n{face, u0 + lu, v0 + lv, lr};
        if (n.radial >= 0 && n.radial < wr) n = p.normalize(n);
        return p.microGet(n, mu, mr, mv);
    };

    for (auto& br : bricks) {
        auto microSolid = [&](int mu, int mr, int mv) {
            return blockProperties(typeAt(br.u, br.v, br.r, mu, mr, mv)).solid;
        };
        for (int mv = 0; mv < N; ++mv) for (int mu = 0; mu < N; ++mu)
            br.alongR[static_cast<std::size_t>(mu + N * mv)] = packMicroAxisBits(N, mu, 0, mv, 2, microSolid);
        for (int mr = 0; mr < N; ++mr) for (int mv = 0; mv < N; ++mv)
            br.alongU[static_cast<std::size_t>(mv + N * mr)] = packMicroAxisBits(N, 0, mr, mv, 0, microSolid);
        for (int mr = 0; mr < N; ++mr) for (int mu = 0; mu < N; ++mu)
            br.alongV[static_cast<std::size_t>(mu + N * mr)] = packMicroAxisBits(N, mu, mr, 0, 1, microSolid);
    }

    auto emitMicro = [&](CellFace f, int u, int v, int r, int mu, int mr, int mv,
                         int w, int h, BlockType type) {
        SurfaceMicroAddress m{{face, u, v, r}, mu, mr, mv};
        const auto q = microFaceCornersExtent(p, m, f, w, h);
        const Vec3 mc = p.microCellCenterPosition(m);
        b.emit(type, q[0], q[1], q[2], q[3],
               normalize(avg4(q[0], q[1], q[2], q[3]) - mc), faceLight(f), true);
    };

    std::vector<char> hasR(static_cast<std::size_t>(wr), 0);
    std::vector<char> hasU(static_cast<std::size_t>(nU), 0);
    std::vector<char> hasV(static_cast<std::size_t>(nV), 0);
    for (const auto& br : bricks) {
        hasR[static_cast<std::size_t>(br.r)] = 1;
        hasU[static_cast<std::size_t>(br.u - u0)] = 1;
        hasV[static_cast<std::size_t>(br.v - v0)] = 1;
    }

    std::vector<GreedyMaskCell> mask;

    for (int r = 0; r < wr; ++r) {
        if (!hasR[static_cast<std::size_t>(r)]) continue;
        int minU = nU, maxU = -1, minV = nV, maxV = -1;
        std::vector<const SnapshotRefinedBrick*> layer;
        for (const auto& br : bricks) if (br.r == r) {
            layer.push_back(&br);
            minU = std::min(minU, br.u - u0);
            maxU = std::max(maxU, br.u - u0);
            minV = std::min(minV, br.v - v0);
            maxV = std::max(maxV, br.v - v0);
        }
        const int wu = (maxU - minU + 1) * N;
        const int wv = (maxV - minV + 1) * N;
        mask.assign(static_cast<std::size_t>(wu * wv), {});
        for (int mr = 0; mr < N; ++mr) {
            const int bit = faceCullBitIndex(mr);
            for (CellFace f : {CellFace::RPos, CellFace::RNeg}) {
                std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
                bool any = false;
                for (const auto* br : layer) {
                    const int iu0 = (br->u - u0 - minU) * N;
                    const int iv0 = (br->v - v0 - minV) * N;
                    for (int mv = 0; mv < N; ++mv) for (int mu = 0; mu < N; ++mu) {
                        const FaceCullWord exposed = (f == CellFace::RPos)
                            ? cullSolidSolidPos(br->alongR[static_cast<std::size_t>(mu + N * mv)])
                            : cullSolidSolidNeg(br->alongR[static_cast<std::size_t>(mu + N * mv)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iu0 + mu + wu * (iv0 + mv))] =
                            {snapshotBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                greedyMask(mask, wu, wv, [&](int iu, int iv, int w, int h, BlockType type) {
                    emitMicro(f, u0 + minU + iu / N, v0 + minV + iv / N, r,
                              iu % N, mr, iv % N, w, h, type);
                });
            }
        }
    }

    for (int lu = 0; lu < nU; ++lu) {
        if (!hasU[static_cast<std::size_t>(lu)]) continue;
        int minV = nV, maxV = -1, minR = wr, maxR = -1;
        std::vector<const SnapshotRefinedBrick*> col;
        for (const auto& br : bricks) if (br.u == u0 + lu) {
            col.push_back(&br);
            minV = std::min(minV, br.v - v0);
            maxV = std::max(maxV, br.v - v0);
            minR = std::min(minR, br.r);
            maxR = std::max(maxR, br.r);
        }
        const int wv = (maxV - minV + 1) * N;
        const int wrn = (maxR - minR + 1) * N;
        mask.assign(static_cast<std::size_t>(wv * wrn), {});
        const int u = u0 + lu;
        for (int mu = 0; mu < N; ++mu) {
            const int bit = faceCullBitIndex(mu);
            for (CellFace f : {CellFace::UPos, CellFace::UNeg}) {
                std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
                bool any = false;
                for (const auto* br : col) {
                    const int iv0 = (br->v - v0 - minV) * N;
                    const int ir0 = (br->r - minR) * N;
                    for (int mr = 0; mr < N; ++mr) for (int mv = 0; mv < N; ++mv) {
                        const FaceCullWord exposed = (f == CellFace::UPos)
                            ? cullSolidSolidPos(br->alongU[static_cast<std::size_t>(mv + N * mr)])
                            : cullSolidSolidNeg(br->alongU[static_cast<std::size_t>(mv + N * mr)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iv0 + mv + wv * (ir0 + mr))] =
                            {snapshotBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                greedyMask(mask, wv, wrn, [&](int iv, int ir, int w, int h, BlockType type) {
                    emitMicro(f, u, v0 + minV + iv / N, minR + ir / N,
                              mu, ir % N, iv % N, w, h, type);
                });
            }
        }
    }

    for (int lv = 0; lv < nV; ++lv) {
        if (!hasV[static_cast<std::size_t>(lv)]) continue;
        int minU = nU, maxU = -1, minR = wr, maxR = -1;
        std::vector<const SnapshotRefinedBrick*> row;
        for (const auto& br : bricks) if (br.v == v0 + lv) {
            row.push_back(&br);
            minU = std::min(minU, br.u - u0);
            maxU = std::max(maxU, br.u - u0);
            minR = std::min(minR, br.r);
            maxR = std::max(maxR, br.r);
        }
        const int wu = (maxU - minU + 1) * N;
        const int wrn = (maxR - minR + 1) * N;
        mask.assign(static_cast<std::size_t>(wu * wrn), {});
        const int v = v0 + lv;
        for (int mv = 0; mv < N; ++mv) {
            const int bit = faceCullBitIndex(mv);
            for (CellFace f : {CellFace::VPos, CellFace::VNeg}) {
                std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
                bool any = false;
                for (const auto* br : row) {
                    const int iu0 = (br->u - u0 - minU) * N;
                    const int ir0 = (br->r - minR) * N;
                    for (int mr = 0; mr < N; ++mr) for (int mu = 0; mu < N; ++mu) {
                        const FaceCullWord exposed = (f == CellFace::VPos)
                            ? cullSolidSolidPos(br->alongV[static_cast<std::size_t>(mu + N * mr)])
                            : cullSolidSolidNeg(br->alongV[static_cast<std::size_t>(mu + N * mr)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iu0 + mu + wu * (ir0 + mr))] =
                            {snapshotBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                greedyMask(mask, wu, wrn, [&](int iu, int ir, int w, int h, BlockType type) {
                    emitMicro(f, u0 + minU + iu / N, v, minR + ir / N,
                              iu % N, ir % N, mv, w, h, type);
                });
            }
        }
    }
}

} // namespace

CpuMeshData buildPlanetSurfaceChunkMesh(const PlanetSurfaceSnapshot& planet,
                                        const PlanetChunkAddress& chunk) {
    Builder builder;
    if (chunk.radial != 0 || chunk.u < 0 || chunk.u >= PlanetSurface::ChunksPerFaceAxis ||
        chunk.v < 0 || chunk.v >= PlanetSurface::ChunksPerFaceAxis) return builder.finish();

    const int u0 = chunk.u * PlanetSurface::ChunkSize;
    const int v0 = chunk.v * PlanetSurface::ChunkSize;
    const int u1 = std::min(u0 + PlanetSurface::ChunkSize, PlanetSurfaceSnapshot::FaceResolution);
    const int v1 = std::min(v0 + PlanetSurface::ChunkSize, PlanetSurfaceSnapshot::FaceResolution);
    const int wu = u1 - u0;
    const int wv = v1 - v0;
    constexpr int wr = PlanetSurfaceSnapshot::RadialLayers;
    constexpr std::array<CellFace,6> faces{CellFace::UPos,CellFace::UNeg,CellFace::VPos,CellFace::VNeg,CellFace::RPos,CellFace::RNeg};

    // Greedy merge coplanar macro faces in cube-sphere face-grid space (P0-19).
    // Candidate faces use P0-21 bitmask solid-solid culls with a one-cell halo
    // so cube-face seams and mantle/sky radial neighbors stay in the word.
    std::vector<GreedyMaskCell> mask;
    const int rBits = wr + kFaceCullHalo * 2;
    const int uBits = wu + kFaceCullHalo * 2;
    const int vBits = wv + kFaceCullHalo * 2;

    // R+/- : fixed r, mask (u,v) — pack along radial.
    std::vector<FaceCullColumn> rCols(static_cast<std::size_t>(wu * wv));
    for (int v = v0; v < v1; ++v) for (int u = u0; u < u1; ++u)
        rCols[static_cast<std::size_t>((u - u0) + wu * (v - v0))] =
            packSnapshotColumn(planet, chunk.face, u, v, 0, 2, -kFaceCullHalo, rBits);
    mask.assign(static_cast<std::size_t>(wu * wv), {});
    for (int r = 0; r < wr; ++r) {
        const int bit = faceCullBitIndex(r);
        for (CellFace face : {CellFace::RPos, CellFace::RNeg}) {
            std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
            for (int v = v0; v < v1; ++v) for (int u = u0; u < u1; ++u) {
                const auto& col = rCols[static_cast<std::size_t>((u - u0) + wu * (v - v0))];
                const FaceCullWord exposed = (face == CellFace::RPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>((u - u0) + wu * (v - v0))] =
                    {planet.get({chunk.face, u, v, r}), true};
            }
            greedyMask(mask, wu, wv, [&](int gu, int gv, int w, int h, BlockType type) {
                emitMacroFaceExtent(builder, planet, {chunk.face, u0 + gu, v0 + gv, r}, face, type, w, h);
            });
        }
    }
    // U+/- : fixed u, mask (v,r) — pack along U (halo wraps via snapshot get/normalize).
    std::vector<FaceCullColumn> uCols(static_cast<std::size_t>(wv * wr));
    for (int r = 0; r < wr; ++r) for (int v = v0; v < v1; ++v)
        uCols[static_cast<std::size_t>((v - v0) + wv * r)] =
            packSnapshotColumn(planet, chunk.face, 0, v, r, 0, u0 - kFaceCullHalo, uBits);
    mask.assign(static_cast<std::size_t>(wv * wr), {});
    for (int u = u0; u < u1; ++u) {
        const int bit = faceCullBitIndex(u - u0);
        for (CellFace face : {CellFace::UPos, CellFace::UNeg}) {
            std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
            for (int r = 0; r < wr; ++r) for (int v = v0; v < v1; ++v) {
                const auto& col = uCols[static_cast<std::size_t>((v - v0) + wv * r)];
                const FaceCullWord exposed = (face == CellFace::UPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>((v - v0) + wv * r)] =
                    {planet.get({chunk.face, u, v, r}), true};
            }
            greedyMask(mask, wv, wr, [&](int gv, int gr, int w, int h, BlockType type) {
                emitMacroFaceExtent(builder, planet, {chunk.face, u, v0 + gv, gr}, face, type, w, h);
            });
        }
    }
    // V+/- : fixed v, mask (u,r) — pack along V.
    std::vector<FaceCullColumn> vCols(static_cast<std::size_t>(wu * wr));
    for (int r = 0; r < wr; ++r) for (int u = u0; u < u1; ++u)
        vCols[static_cast<std::size_t>((u - u0) + wu * r)] =
            packSnapshotColumn(planet, chunk.face, u, 0, r, 1, v0 - kFaceCullHalo, vBits);
    mask.assign(static_cast<std::size_t>(wu * wr), {});
    for (int v = v0; v < v1; ++v) {
        const int bit = faceCullBitIndex(v - v0);
        for (CellFace face : {CellFace::VPos, CellFace::VNeg}) {
            std::fill(mask.begin(), mask.end(), GreedyMaskCell{});
            for (int r = 0; r < wr; ++r) for (int u = u0; u < u1; ++u) {
                const auto& col = vCols[static_cast<std::size_t>((u - u0) + wu * r)];
                const FaceCullWord exposed = (face == CellFace::VPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>((u - u0) + wu * r)] =
                    {planet.get({chunk.face, u, v, r}), true};
            }
            greedyMask(mask, wu, wr, [&](int gu, int gr, int w, int h, BlockType type) {
                emitMacroFaceExtent(builder, planet, {chunk.face, u0 + gu, v, gr}, face, type, w, h);
            });
        }
    }

    // Refined cells + macro→refined tiled boundaries (not greedied across micro).
    // Micro faces themselves merge across adjacent refined bricks in this chunk.
    for(int v=v0;v<v1;++v) for(int u=u0;u<u1;++u) for(int r=0;r<wr;++r) {
        const SurfaceCellAddress a{chunk.face,u,v,r};
        if (planet.hasMicroDetail(a)) continue;
        const BlockType type=planet.get(a);
        if (!blockProperties(type).solid) continue;
        for(const CellFace face:faces) {
            const auto n=neighborOf(planet,a,face);
            if (planet.radialInBounds(n.radial) && planet.hasMicroDetail(n))
                emitTiledMacroBoundary(builder,planet,a,face,type);
        }
    }
    emitCrossBrickMicro(builder,planet,chunk.face,u0,v0,u1,v1);
    return builder.finish();
}


namespace {

SurfaceCellAddress cachedWorldAddress(const SurfaceChunkData& c,int lu,int lv,int lr) {
    return c.worldAddress(lu,lv,lr);
}

Vec3 cachedBoundaryPosition(const SurfaceChunkData& c, CubeFace face, int uEdge, int vEdge, int radialBoundary) {
    const Vec3 d=faceGridCornerDirection(face,uEdge,vEdge,PlanetSurface::FaceResolution);
    const float radius=c.referenceRadius + static_cast<float>(radialBoundary-PlanetSurface::ReferenceRadial);
    return d*radius;
}

Vec3 cachedCellCenter(const SurfaceChunkData& c, SurfaceCellAddress a) {
    if (a.u<0 || a.u>=PlanetSurface::FaceResolution || a.v<0 || a.v>=PlanetSurface::FaceResolution) {
        const auto wrapped=wrapFaceCell(a.face,a.u,a.v,PlanetSurface::FaceResolution);
        a.face=wrapped.face; a.u=wrapped.u; a.v=wrapped.v;
    }
    const Vec3 d=faceGridCellDirection(a.face,a.u,a.v,PlanetSurface::FaceResolution);
    const float radius=c.referenceRadius + static_cast<float>(a.radial)+0.5f-static_cast<float>(PlanetSurface::ReferenceRadial);
    return d*radius;
}

Vec3 cachedMicroBoundary(const SurfaceChunkData& c, SurfaceCellAddress a,
                         int uEdge,int radialEdge,int vEdge) {
    if (a.u<0 || a.u>=PlanetSurface::FaceResolution || a.v<0 || a.v>=PlanetSurface::FaceResolution) {
        const auto wrapped=wrapFaceCell(a.face,a.u,a.v,PlanetSurface::FaceResolution);
        a.face=wrapped.face; a.u=wrapped.u; a.v=wrapped.v;
    }
    const float fu=static_cast<float>(a.u)+static_cast<float>(uEdge)/MicroBrick::Resolution;
    const float fv=static_cast<float>(a.v)+static_cast<float>(vEdge)/MicroBrick::Resolution;
    const float u=fu/static_cast<float>(PlanetSurface::FaceResolution)*2.0f-1.0f;
    const float v=fv/static_cast<float>(PlanetSurface::FaceResolution)*2.0f-1.0f;
    const Vec3 d=faceUvToDirection(a.face,u,v);
    const float rr=static_cast<float>(a.radial)+static_cast<float>(radialEdge)/MicroBrick::Resolution;
    return d*(c.referenceRadius+rr-static_cast<float>(PlanetSurface::ReferenceRadial));
}

Vec3 cachedMicroCenter(const SurfaceChunkData& c,const SurfaceMicroAddress& m) {
    Vec3 sum{};
    for(int du=0;du<=1;++du) for(int dr=0;dr<=1;++dr) for(int dv=0;dv<=1;++dv)
        sum=sum+cachedMicroBoundary(c,m.cell,m.u+du,m.radial+dr,m.v+dv);
    return sum*(1.0f/8.0f);
}

std::array<Vec3,4> cachedMacroCorners(const SurfaceChunkData& c,SurfaceCellAddress a,CellFace face) {
    const int u=a.u,v=a.v,r=a.radial;
    switch(face) {
        case CellFace::RPos: return {cachedBoundaryPosition(c,a.face,u,v,r+1),cachedBoundaryPosition(c,a.face,u+1,v,r+1),cachedBoundaryPosition(c,a.face,u+1,v+1,r+1),cachedBoundaryPosition(c,a.face,u,v+1,r+1)};
        case CellFace::RNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v+1,r),cachedBoundaryPosition(c,a.face,u+1,v+1,r),cachedBoundaryPosition(c,a.face,u+1,v,r)};
        case CellFace::UPos: return {cachedBoundaryPosition(c,a.face,u+1,v,r),cachedBoundaryPosition(c,a.face,u+1,v,r+1),cachedBoundaryPosition(c,a.face,u+1,v+1,r+1),cachedBoundaryPosition(c,a.face,u+1,v+1,r)};
        case CellFace::UNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v+1,r),cachedBoundaryPosition(c,a.face,u,v+1,r+1),cachedBoundaryPosition(c,a.face,u,v,r+1)};
        case CellFace::VPos: return {cachedBoundaryPosition(c,a.face,u,v+1,r),cachedBoundaryPosition(c,a.face,u+1,v+1,r),cachedBoundaryPosition(c,a.face,u+1,v+1,r+1),cachedBoundaryPosition(c,a.face,u,v+1,r+1)};
        case CellFace::VNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v,r+1),cachedBoundaryPosition(c,a.face,u+1,v,r+1),cachedBoundaryPosition(c,a.face,u+1,v,r)};
    }
    return {};
}

std::array<Vec3,4> cachedMicroCorners(const SurfaceChunkData& c,const SurfaceMicroAddress& m,CellFace face) {
    const int u=m.u,v=m.v,r=m.radial;
    switch(face) {
        case CellFace::RPos: return {cachedMicroBoundary(c,m.cell,u,r+1,v),cachedMicroBoundary(c,m.cell,u+1,r+1,v),cachedMicroBoundary(c,m.cell,u+1,r+1,v+1),cachedMicroBoundary(c,m.cell,u,r+1,v+1)};
        case CellFace::RNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r,v+1),cachedMicroBoundary(c,m.cell,u+1,r,v+1),cachedMicroBoundary(c,m.cell,u+1,r,v)};
        case CellFace::UPos: return {cachedMicroBoundary(c,m.cell,u+1,r,v),cachedMicroBoundary(c,m.cell,u+1,r+1,v),cachedMicroBoundary(c,m.cell,u+1,r+1,v+1),cachedMicroBoundary(c,m.cell,u+1,r,v+1)};
        case CellFace::UNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r,v+1),cachedMicroBoundary(c,m.cell,u,r+1,v+1),cachedMicroBoundary(c,m.cell,u,r+1,v)};
        case CellFace::VPos: return {cachedMicroBoundary(c,m.cell,u,r,v+1),cachedMicroBoundary(c,m.cell,u+1,r,v+1),cachedMicroBoundary(c,m.cell,u+1,r+1,v+1),cachedMicroBoundary(c,m.cell,u,r+1,v+1)};
        case CellFace::VNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r+1,v),cachedMicroBoundary(c,m.cell,u+1,r+1,v),cachedMicroBoundary(c,m.cell,u+1,r,v)};
    }
    return {};
}

void faceDelta(CellFace face,int& du,int& dv,int& dr) {
    du=dv=dr=0;
    switch(face) {
        case CellFace::UPos: du=1; break;
        case CellFace::UNeg: du=-1; break;
        case CellFace::VPos: dv=1; break;
        case CellFace::VNeg: dv=-1; break;
        case CellFace::RPos: dr=1; break;
        case CellFace::RNeg: dr=-1; break;
    }
}

BlockType cachedBoundaryMicro(const SurfaceChunkData& c,int lu,int lv,int lr,
                              int mu,int mr,int mv,CellFace face) {
    int nmu=mu,nmr=mr,nmv=mv;
    int du=0,dv=0,dr=0;
    switch(face) {
        case CellFace::UPos: ++nmu; if(nmu>=MicroBrick::Resolution){nmu=0;du=1;} break;
        case CellFace::UNeg: --nmu; if(nmu<0){nmu=MicroBrick::Resolution-1;du=-1;} break;
        case CellFace::VPos: ++nmv; if(nmv>=MicroBrick::Resolution){nmv=0;dv=1;} break;
        case CellFace::VNeg: --nmv; if(nmv<0){nmv=MicroBrick::Resolution-1;dv=-1;} break;
        case CellFace::RPos: ++nmr; if(nmr>=MicroBrick::Resolution){nmr=0;dr=1;} break;
        case CellFace::RNeg: --nmr; if(nmr<0){nmr=MicroBrick::Resolution-1;dr=-1;} break;
    }
    if(du==0&&dv==0&&dr==0)
        return c.microGetLocal(lu,lv,lr,nmu,nmr,nmv);
    return c.microGetLocal(lu+du,lv+dv,lr+dr,nmu,nmr,nmv);
}

struct GridDelta { int u{}; int v{}; int r{}; };

std::pair<GridDelta,GridDelta> faceTangents(CellFace face) {
    switch(face) {
        case CellFace::RPos: case CellFace::RNeg: return {{1,0,0},{0,1,0}};
        case CellFace::UPos: case CellFace::UNeg: return {{0,0,1},{0,1,0}};
        case CellFace::VPos: case CellFace::VNeg: return {{1,0,0},{0,0,1}};
    }
    return {};
}

std::array<std::pair<int,int>,4> faceCornerSigns(CellFace face) {
    constexpr std::array<std::pair<int,int>,4> positive{{{-1,-1},{1,-1},{1,1},{-1,1}}};
    constexpr std::array<std::pair<int,int>,4> negative{{{-1,-1},{-1,1},{1,1},{1,-1}}};
    switch(face) {
        case CellFace::RPos: case CellFace::UPos: case CellFace::VPos: return positive;
        case CellFace::RNeg: case CellFace::UNeg: case CellFace::VNeg: return negative;
    }
    return positive;
}

float aoFromOccupancy(int occupied) {
    constexpr std::array<float,4> factors{1.0f,0.86f,0.72f,0.58f};
    return factors[static_cast<std::size_t>(std::clamp(occupied,0,3))];
}

std::array<float,4> cachedMacroAo(const SurfaceChunkData& c,int lu,int lv,int lr,CellFace face) {
    int nu{},nv{},nr{};
    faceDelta(face,nu,nv,nr);
    const auto [ta,tb]=faceTangents(face);
    const auto signs=faceCornerSigns(face);
    std::array<float,4> out{};
    auto solid=[&](int u,int v,int r) { return blockProperties(c.getWithHalo(u,v,r)).solid; };
    for(std::size_t i=0;i<out.size();++i) {
        const auto [sa,sb]=signs[i];
        const int bu=lu+nu,bv=lv+nv,br=lr+nr;
        int occupied=0;
        occupied+=solid(bu+ta.u*sa,bv+ta.v*sa,br+ta.r*sa)?1:0;
        occupied+=solid(bu+tb.u*sb,bv+tb.v*sb,br+tb.r*sb)?1:0;
        occupied+=solid(bu+ta.u*sa+tb.u*sb,bv+ta.v*sa+tb.v*sb,br+ta.r*sa+tb.r*sb)?1:0;
        out[i]=aoFromOccupancy(occupied);
    }
    return out;
}

BlockType cachedMicroOffset(const SurfaceChunkData& c,int lu,int lv,int lr,
                            int mu,int mr,int mv,int du,int dr,int dv) {
    constexpr int N=MicroBrick::Resolution;
    mu+=du; mr+=dr; mv+=dv;
    while(mu<0){mu+=N;--lu;} while(mu>=N){mu-=N;++lu;}
    while(mv<0){mv+=N;--lv;} while(mv>=N){mv-=N;++lv;}
    while(mr<0){mr+=N;--lr;} while(mr>=N){mr-=N;++lr;}
    return c.microGetLocal(lu,lv,lr,mu,mr,mv);
}

std::array<float,4> cachedMicroAo(const SurfaceChunkData& c,int lu,int lv,int lr,
                                  int mu,int mr,int mv,CellFace face) {
    int nu{},nv{},nr{};
    faceDelta(face,nu,nv,nr);
    const auto [ta,tb]=faceTangents(face);
    const auto signs=faceCornerSigns(face);
    std::array<float,4> out{};
    auto solid=[&](int du,int dv,int dr) {
        return blockProperties(cachedMicroOffset(c,lu,lv,lr,mu,mr,mv,du,dr,dv)).solid;
    };
    for(std::size_t i=0;i<out.size();++i) {
        const auto [sa,sb]=signs[i];
        int occupied=0;
        occupied+=solid(nu+ta.u*sa,nv+ta.v*sa,nr+ta.r*sa)?1:0;
        occupied+=solid(nu+tb.u*sb,nv+tb.v*sb,nr+tb.r*sb)?1:0;
        occupied+=solid(nu+ta.u*sa+tb.u*sb,nv+ta.v*sa+tb.v*sb,nr+ta.r*sa+tb.r*sb)?1:0;
        out[i]=aoFromOccupancy(occupied);
    }
    return out;
}

std::array<Vec3,4> cachedMacroCornersExtent(const SurfaceChunkData& c,SurfaceCellAddress a,
                                            CellFace face,int w,int h) {
    const int u=a.u,v=a.v,r=a.radial;
    switch(face) {
        case CellFace::RPos: return {cachedBoundaryPosition(c,a.face,u,v,r+1),cachedBoundaryPosition(c,a.face,u+w,v,r+1),cachedBoundaryPosition(c,a.face,u+w,v+h,r+1),cachedBoundaryPosition(c,a.face,u,v+h,r+1)};
        case CellFace::RNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v+h,r),cachedBoundaryPosition(c,a.face,u+w,v+h,r),cachedBoundaryPosition(c,a.face,u+w,v,r)};
        case CellFace::UPos: return {cachedBoundaryPosition(c,a.face,u+1,v,r),cachedBoundaryPosition(c,a.face,u+1,v,r+h),cachedBoundaryPosition(c,a.face,u+1,v+w,r+h),cachedBoundaryPosition(c,a.face,u+1,v+w,r)};
        case CellFace::UNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v+w,r),cachedBoundaryPosition(c,a.face,u,v+w,r+h),cachedBoundaryPosition(c,a.face,u,v,r+h)};
        case CellFace::VPos: return {cachedBoundaryPosition(c,a.face,u,v+1,r),cachedBoundaryPosition(c,a.face,u+w,v+1,r),cachedBoundaryPosition(c,a.face,u+w,v+1,r+h),cachedBoundaryPosition(c,a.face,u,v+1,r+h)};
        case CellFace::VNeg: return {cachedBoundaryPosition(c,a.face,u,v,r),cachedBoundaryPosition(c,a.face,u,v,r+h),cachedBoundaryPosition(c,a.face,u+w,v,r+h),cachedBoundaryPosition(c,a.face,u+w,v,r)};
    }
    return {};
}

std::array<float,4> cachedMacroAoExtent(const SurfaceChunkData& c,int lu,int lv,int lr,
                                        CellFace face,int w,int h) {
    // Corner cells follow the same winding as cachedMacroCornersExtent.
    int c0u=lu,c0v=lv,c0r=lr;
    int c1u=lu,c1v=lv,c1r=lr;
    int c2u=lu,c2v=lv,c2r=lr;
    int c3u=lu,c3v=lv,c3r=lr;
    switch(face) {
        case CellFace::RPos:
            c1u=lu+w-1; c1v=lv;     c1r=lr;
            c2u=lu+w-1; c2v=lv+h-1; c2r=lr;
            c3u=lu;     c3v=lv+h-1; c3r=lr;
            break;
        case CellFace::RNeg:
            c1u=lu;     c1v=lv+h-1; c1r=lr;
            c2u=lu+w-1; c2v=lv+h-1; c2r=lr;
            c3u=lu+w-1; c3v=lv;     c3r=lr;
            break;
        case CellFace::UPos:
            c1u=lu; c1v=lv;     c1r=lr+h-1;
            c2u=lu; c2v=lv+w-1; c2r=lr+h-1;
            c3u=lu; c3v=lv+w-1; c3r=lr;
            break;
        case CellFace::UNeg:
            c1u=lu; c1v=lv+w-1; c1r=lr;
            c2u=lu; c2v=lv+w-1; c2r=lr+h-1;
            c3u=lu; c3v=lv;     c3r=lr+h-1;
            break;
        case CellFace::VPos:
            c1u=lu+w-1; c1v=lv; c1r=lr;
            c2u=lu+w-1; c2v=lv; c2r=lr+h-1;
            c3u=lu;     c3v=lv; c3r=lr+h-1;
            break;
        case CellFace::VNeg:
            c1u=lu;     c1v=lv; c1r=lr+h-1;
            c2u=lu+w-1; c2v=lv; c2r=lr+h-1;
            c3u=lu+w-1; c3v=lv; c3r=lr;
            break;
    }
    const auto a0=cachedMacroAo(c,c0u,c0v,c0r,face);
    const auto a1=cachedMacroAo(c,c1u,c1v,c1r,face);
    const auto a2=cachedMacroAo(c,c2u,c2v,c2r,face);
    const auto a3=cachedMacroAo(c,c3u,c3v,c3r,face);
    return {a0[0],a1[1],a2[2],a3[3]};
}

void emitCachedMacroFaceExtent(Builder& b,const SurfaceChunkData& c,int lu,int lv,int lr,
                               SurfaceCellAddress a,CellFace face,BlockType type,int w,int h) {
    const auto q=cachedMacroCornersExtent(c,a,face,w,h);
    const Vec3 center=cachedCellCenter(c,a);
    b.emitAo(type,q[0],q[1],q[2],q[3],normalize(avg4(q[0],q[1],q[2],q[3])-center),
             faceLight(face),false,cachedMacroAoExtent(c,lu,lv,lr,face,w,h));
}

void emitCachedTiledBoundary(Builder& b,const SurfaceChunkData& c,int lu,int lv,int lr,
                             SurfaceCellAddress a,CellFace face,BlockType type) {
    constexpr int N=MicroBrick::Resolution;
    for(int i=0;i<N;++i) for(int j=0;j<N;++j) {
        int mu=0,mr=0,mv=0;
        switch(face) {
            case CellFace::RPos: mu=i;mr=N-1;mv=j; break;
            case CellFace::RNeg: mu=i;mr=0;mv=j; break;
            case CellFace::UPos: mu=N-1;mr=i;mv=j; break;
            case CellFace::UNeg: mu=0;mr=i;mv=j; break;
            case CellFace::VPos: mu=i;mr=j;mv=N-1; break;
            case CellFace::VNeg: mu=i;mr=j;mv=0; break;
        }
        if(blockProperties(cachedBoundaryMicro(c,lu,lv,lr,mu,mr,mv,face)).solid) continue;
        SurfaceMicroAddress m{a,mu,mr,mv};
        const auto q=cachedMicroCorners(c,m,face);
        const Vec3 mc=cachedMicroCenter(c,m);
        b.emitAo(type,q[0],q[1],q[2],q[3],normalize(avg4(q[0],q[1],q[2],q[3])-mc),
                 faceLight(face),true,cachedMicroAo(c,lu,lv,lr,mu,mr,mv,face));
    }
}

std::array<Vec3,4> cachedMicroCornersExtent(const SurfaceChunkData& c,const SurfaceMicroAddress& m,
                                            CellFace face,int w,int h) {
    const int u=m.u,v=m.v,r=m.radial;
    switch(face) {
        case CellFace::RPos: return {cachedMicroBoundary(c,m.cell,u,r+1,v),cachedMicroBoundary(c,m.cell,u+w,r+1,v),cachedMicroBoundary(c,m.cell,u+w,r+1,v+h),cachedMicroBoundary(c,m.cell,u,r+1,v+h)};
        case CellFace::RNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r,v+h),cachedMicroBoundary(c,m.cell,u+w,r,v+h),cachedMicroBoundary(c,m.cell,u+w,r,v)};
        case CellFace::UPos: return {cachedMicroBoundary(c,m.cell,u+1,r,v),cachedMicroBoundary(c,m.cell,u+1,r+h,v),cachedMicroBoundary(c,m.cell,u+1,r+h,v+w),cachedMicroBoundary(c,m.cell,u+1,r,v+w)};
        case CellFace::UNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r,v+w),cachedMicroBoundary(c,m.cell,u,r+h,v+w),cachedMicroBoundary(c,m.cell,u,r+h,v)};
        case CellFace::VPos: return {cachedMicroBoundary(c,m.cell,u,r,v+1),cachedMicroBoundary(c,m.cell,u+w,r,v+1),cachedMicroBoundary(c,m.cell,u+w,r+h,v+1),cachedMicroBoundary(c,m.cell,u,r+h,v+1)};
        case CellFace::VNeg: return {cachedMicroBoundary(c,m.cell,u,r,v),cachedMicroBoundary(c,m.cell,u,r+h,v),cachedMicroBoundary(c,m.cell,u+w,r+h,v),cachedMicroBoundary(c,m.cell,u+w,r,v)};
    }
    return {};
}

std::array<float,4> cachedMicroAoExtent(const SurfaceChunkData& c,int lu,int lv,int lr,
                                        int mu,int mr,int mv,CellFace face,int w,int h) {
    int c0u=mu,c0r=mr,c0v=mv;
    int c1u=mu,c1r=mr,c1v=mv;
    int c2u=mu,c2r=mr,c2v=mv;
    int c3u=mu,c3r=mr,c3v=mv;
    switch(face) {
        case CellFace::RPos:
            c1u=mu+w-1; c1v=mv;     c1r=mr;
            c2u=mu+w-1; c2v=mv+h-1; c2r=mr;
            c3u=mu;     c3v=mv+h-1; c3r=mr;
            break;
        case CellFace::RNeg:
            c1u=mu;     c1v=mv+h-1; c1r=mr;
            c2u=mu+w-1; c2v=mv+h-1; c2r=mr;
            c3u=mu+w-1; c3v=mv;     c3r=mr;
            break;
        case CellFace::UPos:
            c1u=mu; c1v=mv;     c1r=mr+h-1;
            c2u=mu; c2v=mv+w-1; c2r=mr+h-1;
            c3u=mu; c3v=mv+w-1; c3r=mr;
            break;
        case CellFace::UNeg:
            c1u=mu; c1v=mv+w-1; c1r=mr;
            c2u=mu; c2v=mv+w-1; c2r=mr+h-1;
            c3u=mu; c3v=mv;     c3r=mr+h-1;
            break;
        case CellFace::VPos:
            c1u=mu+w-1; c1v=mv; c1r=mr;
            c2u=mu+w-1; c2v=mv; c2r=mr+h-1;
            c3u=mu;     c3v=mv; c3r=mr+h-1;
            break;
        case CellFace::VNeg:
            c1u=mu;     c1v=mv; c1r=mr+h-1;
            c2u=mu+w-1; c2v=mv; c2r=mr+h-1;
            c3u=mu+w-1; c3v=mv; c3r=mr;
            break;
    }
    const auto a0=cachedMicroAo(c,lu,lv,lr,c0u,c0r,c0v,face);
    const auto a1=cachedMicroAo(c,lu,lv,lr,c1u,c1r,c1v,face);
    const auto a2=cachedMicroAo(c,lu,lv,lr,c2u,c2r,c2v,face);
    const auto a3=cachedMicroAo(c,lu,lv,lr,c3u,c3r,c3v,face);
    return {a0[0],a1[1],a2[2],a3[3]};
}

// Second anonymous-namespace copy: TU-local anonymous namespaces do not share
// symbols across `namespace { }` blocks, so the greedy helpers are duplicated.
struct CachedGreedyMaskCell {
    BlockType type{BlockType::Air};
    bool active{};
};

template <class Emit>
void cachedGreedyMask(std::vector<CachedGreedyMaskCell>& mask, int width, int height, Emit&& emit) {
    for (int v = 0; v < height; ++v) {
        for (int u = 0; u < width; ++u) {
            const int start = u + width * v;
            if (!mask[static_cast<std::size_t>(start)].active) continue;
            const BlockType type = mask[static_cast<std::size_t>(start)].type;
            int runW = 1;
            while (u + runW < width) {
                const auto& cell = mask[static_cast<std::size_t>(u + runW + width * v)];
                if (!cell.active || cell.type != type) break;
                ++runW;
            }
            int runH = 1;
            bool canGrow = true;
            while (v + runH < height && canGrow) {
                for (int du = 0; du < runW; ++du) {
                    const auto& cell = mask[static_cast<std::size_t>(u + du + width * (v + runH))];
                    if (!cell.active || cell.type != type) { canGrow = false; break; }
                }
                if (canGrow) ++runH;
            }
            emit(u, v, runW, runH, type);
            for (int dv = 0; dv < runH; ++dv)
                for (int du = 0; du < runW; ++du)
                    mask[static_cast<std::size_t>(u + du + width * (v + dv))].active = false;
        }
    }
}

FaceCullColumn packCachedColumn(const SurfaceChunkData& chunk, int lu, int lv, int lr,
                                int axis, int origin, int count) {
    auto localAt = [&](int i, int& x, int& y, int& z) {
        x = lu; y = lv; z = lr;
        const int p = origin + i;
        if (axis == 0) x = p;
        else if (axis == 1) y = p;
        else z = p;
    };
    FaceCullColumn col{};
    col.solid = packPredBits(count, [&](int i) {
        int x=0,y=0,z=0; localAt(i,x,y,z);
        return blockProperties(chunk.getWithHalo(x,y,z)).solid;
    });
    col.refined = packPredBits(count, [&](int i) {
        int x=0,y=0,z=0; localAt(i,x,y,z);
        return chunk.hasMicroDetail(cachedWorldAddress(chunk,x,y,z));
    });
    return col;
}

struct CachedRefinedBrick {
    int lu{};
    int lv{};
    int lr{};
    std::array<BlockType, MicroBrick::CellCount> cells{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongR{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongU{};
    std::array<FaceCullWord, MicroBrick::CellCount / MicroBrick::Resolution> alongV{};
};

BlockType cachedBrickCell(const CachedRefinedBrick& b, int mu, int mr, int mv) {
    return b.cells[static_cast<std::size_t>(MicroBrick::index(mu, mr, mv))];
}

// P0-20 cached path: stitch refined MicroBricks in the occupancy AABB into one
// greedy mask per face plane. Exposure stays P0-21 per-brick bitmask (+halo).
void emitCachedCrossBrickMicro(Builder& b, const SurfaceChunkData& c,
                               int lu0, int lv0, int lr0, int lu1, int lv1, int lr1) {
    constexpr int N = MicroBrick::Resolution;
    const int nU = lu1 - lu0;
    const int nV = lv1 - lv0;
    const int nR = lr1 - lr0;
    if (nU <= 0 || nV <= 0 || nR <= 0) return;

    std::vector<CachedRefinedBrick> bricks;
    std::unordered_map<int, std::size_t> indexOf;
    auto pack = [&](int lu, int lv, int lr) {
        return (lu - lu0) + nU * ((lv - lv0) + nV * (lr - lr0));
    };
    for (int lv = lv0; lv < lv1; ++lv)
        for (int lu = lu0; lu < lu1; ++lu)
            for (int lr = lr0; lr < lr1; ++lr) {
                const SurfaceCellAddress a = cachedWorldAddress(c, lu, lv, lr);
                if (!c.hasMicroDetail(a)) continue;
                CachedRefinedBrick brick;
                brick.lu = lu;
                brick.lv = lv;
                brick.lr = lr;
                c.sampleMicroBrickLocal(lu, lv, lr, brick.cells);
                indexOf[pack(lu, lv, lr)] = bricks.size();
                bricks.push_back(brick);
            }
    if (bricks.empty()) return;

    auto typeAt = [&](int lu, int lv, int lr, int mu, int mr, int mv) -> BlockType {
        while (mu < 0) { mu += N; --lu; } while (mu >= N) { mu -= N; ++lu; }
        while (mv < 0) { mv += N; --lv; } while (mv >= N) { mv -= N; ++lv; }
        while (mr < 0) { mr += N; --lr; } while (mr >= N) { mr -= N; ++lr; }
        if (lu >= lu0 && lu < lu1 && lv >= lv0 && lv < lv1 && lr >= lr0 && lr < lr1) {
            const auto it = indexOf.find(pack(lu, lv, lr));
            if (it != indexOf.end())
                return cachedBrickCell(bricks[it->second], mu, mr, mv);
        }
        return c.microGetLocal(lu, lv, lr, mu, mr, mv);
    };

    for (auto& br : bricks) {
        auto microSolid = [&](int mu, int mr, int mv) {
            return blockProperties(typeAt(br.lu, br.lv, br.lr, mu, mr, mv)).solid;
        };
        for (int mv = 0; mv < N; ++mv) for (int mu = 0; mu < N; ++mu)
            br.alongR[static_cast<std::size_t>(mu + N * mv)] = packMicroAxisBits(N, mu, 0, mv, 2, microSolid);
        for (int mr = 0; mr < N; ++mr) for (int mv = 0; mv < N; ++mv)
            br.alongU[static_cast<std::size_t>(mv + N * mr)] = packMicroAxisBits(N, 0, mr, mv, 0, microSolid);
        for (int mr = 0; mr < N; ++mr) for (int mu = 0; mu < N; ++mu)
            br.alongV[static_cast<std::size_t>(mu + N * mr)] = packMicroAxisBits(N, mu, mr, 0, 1, microSolid);
    }

    auto emitMicro = [&](CellFace f, int lu, int lv, int lr, int mu, int mr, int mv,
                         int w, int h, BlockType type) {
        const auto a = cachedWorldAddress(c, lu, lv, lr);
        SurfaceMicroAddress m{a, mu, mr, mv};
        const auto q = cachedMicroCornersExtent(c, m, f, w, h);
        const Vec3 mc = cachedMicroCenter(c, m);
        b.emitAo(type, q[0], q[1], q[2], q[3],
                 normalize(avg4(q[0], q[1], q[2], q[3]) - mc),
                 faceLight(f), true, cachedMicroAoExtent(c, lu, lv, lr, mu, mr, mv, f, w, h));
    };

    std::vector<char> hasR(static_cast<std::size_t>(nR), 0);
    std::vector<char> hasU(static_cast<std::size_t>(nU), 0);
    std::vector<char> hasV(static_cast<std::size_t>(nV), 0);
    for (const auto& br : bricks) {
        hasR[static_cast<std::size_t>(br.lr - lr0)] = 1;
        hasU[static_cast<std::size_t>(br.lu - lu0)] = 1;
        hasV[static_cast<std::size_t>(br.lv - lv0)] = 1;
    }

    std::vector<CachedGreedyMaskCell> mask;

    for (int lr = lr0; lr < lr1; ++lr) {
        if (!hasR[static_cast<std::size_t>(lr - lr0)]) continue;
        int minU = nU, maxU = -1, minV = nV, maxV = -1;
        std::vector<const CachedRefinedBrick*> layer;
        for (const auto& br : bricks) if (br.lr == lr) {
            layer.push_back(&br);
            minU = std::min(minU, br.lu - lu0);
            maxU = std::max(maxU, br.lu - lu0);
            minV = std::min(minV, br.lv - lv0);
            maxV = std::max(maxV, br.lv - lv0);
        }
        const int wu = (maxU - minU + 1) * N;
        const int wv = (maxV - minV + 1) * N;
        mask.assign(static_cast<std::size_t>(wu * wv), {});
        for (int mr = 0; mr < N; ++mr) {
            const int bit = faceCullBitIndex(mr);
            for (CellFace f : {CellFace::RPos, CellFace::RNeg}) {
                std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
                bool any = false;
                for (const auto* br : layer) {
                    const int iu0 = (br->lu - lu0 - minU) * N;
                    const int iv0 = (br->lv - lv0 - minV) * N;
                    for (int mv = 0; mv < N; ++mv) for (int mu = 0; mu < N; ++mu) {
                        const FaceCullWord exposed = (f == CellFace::RPos)
                            ? cullSolidSolidPos(br->alongR[static_cast<std::size_t>(mu + N * mv)])
                            : cullSolidSolidNeg(br->alongR[static_cast<std::size_t>(mu + N * mv)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iu0 + mu + wu * (iv0 + mv))] =
                            {cachedBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                cachedGreedyMask(mask, wu, wv, [&](int iu, int iv, int w, int h, BlockType type) {
                    emitMicro(f, lu0 + minU + iu / N, lv0 + minV + iv / N, lr,
                              iu % N, mr, iv % N, w, h, type);
                });
            }
        }
    }

    for (int lu = lu0; lu < lu1; ++lu) {
        if (!hasU[static_cast<std::size_t>(lu - lu0)]) continue;
        int minV = nV, maxV = -1, minR = nR, maxR = -1;
        std::vector<const CachedRefinedBrick*> col;
        for (const auto& br : bricks) if (br.lu == lu) {
            col.push_back(&br);
            minV = std::min(minV, br.lv - lv0);
            maxV = std::max(maxV, br.lv - lv0);
            minR = std::min(minR, br.lr - lr0);
            maxR = std::max(maxR, br.lr - lr0);
        }
        const int wv = (maxV - minV + 1) * N;
        const int wrn = (maxR - minR + 1) * N;
        mask.assign(static_cast<std::size_t>(wv * wrn), {});
        for (int mu = 0; mu < N; ++mu) {
            const int bit = faceCullBitIndex(mu);
            for (CellFace f : {CellFace::UPos, CellFace::UNeg}) {
                std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
                bool any = false;
                for (const auto* br : col) {
                    const int iv0 = (br->lv - lv0 - minV) * N;
                    const int ir0 = (br->lr - lr0 - minR) * N;
                    for (int mr = 0; mr < N; ++mr) for (int mv = 0; mv < N; ++mv) {
                        const FaceCullWord exposed = (f == CellFace::UPos)
                            ? cullSolidSolidPos(br->alongU[static_cast<std::size_t>(mv + N * mr)])
                            : cullSolidSolidNeg(br->alongU[static_cast<std::size_t>(mv + N * mr)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iv0 + mv + wv * (ir0 + mr))] =
                            {cachedBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                cachedGreedyMask(mask, wv, wrn, [&](int iv, int ir, int w, int h, BlockType type) {
                    emitMicro(f, lu, lv0 + minV + iv / N, lr0 + minR + ir / N,
                              mu, ir % N, iv % N, w, h, type);
                });
            }
        }
    }

    for (int lv = lv0; lv < lv1; ++lv) {
        if (!hasV[static_cast<std::size_t>(lv - lv0)]) continue;
        int minU = nU, maxU = -1, minR = nR, maxR = -1;
        std::vector<const CachedRefinedBrick*> row;
        for (const auto& br : bricks) if (br.lv == lv) {
            row.push_back(&br);
            minU = std::min(minU, br.lu - lu0);
            maxU = std::max(maxU, br.lu - lu0);
            minR = std::min(minR, br.lr - lr0);
            maxR = std::max(maxR, br.lr - lr0);
        }
        const int wu = (maxU - minU + 1) * N;
        const int wrn = (maxR - minR + 1) * N;
        mask.assign(static_cast<std::size_t>(wu * wrn), {});
        for (int mv = 0; mv < N; ++mv) {
            const int bit = faceCullBitIndex(mv);
            for (CellFace f : {CellFace::VPos, CellFace::VNeg}) {
                std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
                bool any = false;
                for (const auto* br : row) {
                    const int iu0 = (br->lu - lu0 - minU) * N;
                    const int ir0 = (br->lr - lr0 - minR) * N;
                    for (int mr = 0; mr < N; ++mr) for (int mu = 0; mu < N; ++mu) {
                        const FaceCullWord exposed = (f == CellFace::VPos)
                            ? cullSolidSolidPos(br->alongV[static_cast<std::size_t>(mu + N * mr)])
                            : cullSolidSolidNeg(br->alongV[static_cast<std::size_t>(mu + N * mr)]);
                        if (!faceCullTest(exposed, bit)) continue;
                        mask[static_cast<std::size_t>(iu0 + mu + wu * (ir0 + mr))] =
                            {cachedBrickCell(*br, mu, mr, mv), true};
                        any = true;
                    }
                }
                if (!any) continue;
                cachedGreedyMask(mask, wu, wrn, [&](int iu, int ir, int w, int h, BlockType type) {
                    emitMicro(f, lu0 + minU + iu / N, lv, lr0 + minR + ir / N,
                              iu % N, ir % N, mv, w, h, type);
                });
            }
        }
    }
}

} // namespace

CpuMeshData buildPlanetSurfaceChunkMesh(const SurfaceChunkData& chunk) {
    Builder builder;
    if(chunk.address.radial<0 || chunk.address.radial>=PlanetSurface::RadialChunks ||
       chunk.address.u<0 || chunk.address.u>=PlanetSurface::ChunksPerFaceAxis ||
       chunk.address.v<0 || chunk.address.v>=PlanetSurface::ChunksPerFaceAxis) return builder.finish();

    // P0-6 / P0-22: Empty adaptive state emits no geometry without scanning the core.
    if(chunk.occupancy.isEmpty()) return builder.finish();

    // Occupancy extents are chunk-local (U=X, V=Y, Radial=Z). Restrict greedy
    // masks and refined passes to the solid AABB; neighbors still use the halo.
    const auto& ext = chunk.occupancy.extents;
    const int lv0 = ext.any ? ext.minY : 0;
    const int lv1 = ext.any ? ext.maxY + 1 : PlanetSurface::ChunkSize;
    const int lu0 = ext.any ? ext.minX : 0;
    const int lu1 = ext.any ? ext.maxX + 1 : PlanetSurface::ChunkSize;
    const int lr0 = ext.any ? ext.minZ : 0;
    const int lr1 = ext.any ? ext.maxZ + 1 : PlanetSurface::ChunkSize;

    constexpr int S=PlanetSurface::ChunkSize;
    constexpr int kColBits = S + kFaceCullHalo * 2;
    constexpr std::array<CellFace,6> faces{CellFace::UPos,CellFace::UNeg,CellFace::VPos,CellFace::VNeg,CellFace::RPos,CellFace::RNeg};
    std::vector<CachedGreedyMaskCell> mask(static_cast<std::size_t>(S*S));
    const int nU = lu1 - lu0;
    const int nV = lv1 - lv0;
    const int nR = lr1 - lr0;

    // R+/- : fixed lr, mask (lu,lv) — P0-19 spherical macro greedy + P0-21 bitmask cull.
    std::vector<FaceCullColumn> rCols(static_cast<std::size_t>(nU * nV));
    for (int lv = lv0; lv < lv1; ++lv) for (int lu = lu0; lu < lu1; ++lu)
        rCols[static_cast<std::size_t>((lu - lu0) + nU * (lv - lv0))] =
            packCachedColumn(chunk, lu, lv, 0, 2, -kFaceCullHalo, kColBits);
    for (int lr = lr0; lr < lr1; ++lr) {
        const int bit = faceCullBitIndex(lr);
        for (CellFace face : {CellFace::RPos, CellFace::RNeg}) {
            std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
            for (int lv = lv0; lv < lv1; ++lv) for (int lu = lu0; lu < lu1; ++lu) {
                const auto& col = rCols[static_cast<std::size_t>((lu - lu0) + nU * (lv - lv0))];
                const FaceCullWord exposed = (face == CellFace::RPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>(lu + S * lv)] = {chunk.getLocal(lu, lv, lr), true};
            }
            cachedGreedyMask(mask, S, S, [&](int gu, int gv, int w, int h, BlockType type) {
                const auto a = cachedWorldAddress(chunk, gu, gv, lr);
                emitCachedMacroFaceExtent(builder, chunk, gu, gv, lr, a, face, type, w, h);
            });
        }
    }
    // U+/- : fixed lu, mask (lv,lr) — pack along U including one-cell halo.
    std::vector<FaceCullColumn> uCols(static_cast<std::size_t>(nV * nR));
    for (int lr = lr0; lr < lr1; ++lr) for (int lv = lv0; lv < lv1; ++lv)
        uCols[static_cast<std::size_t>((lv - lv0) + nV * (lr - lr0))] =
            packCachedColumn(chunk, 0, lv, lr, 0, -kFaceCullHalo, kColBits);
    for (int lu = lu0; lu < lu1; ++lu) {
        const int bit = faceCullBitIndex(lu);
        for (CellFace face : {CellFace::UPos, CellFace::UNeg}) {
            std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
            for (int lr = lr0; lr < lr1; ++lr) for (int lv = lv0; lv < lv1; ++lv) {
                const auto& col = uCols[static_cast<std::size_t>((lv - lv0) + nV * (lr - lr0))];
                const FaceCullWord exposed = (face == CellFace::UPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>(lv + S * lr)] = {chunk.getLocal(lu, lv, lr), true};
            }
            cachedGreedyMask(mask, S, S, [&](int gv, int gr, int w, int h, BlockType type) {
                const auto a = cachedWorldAddress(chunk, lu, gv, gr);
                emitCachedMacroFaceExtent(builder, chunk, lu, gv, gr, a, face, type, w, h);
            });
        }
    }
    // V+/- : fixed lv, mask (lu,lr)
    std::vector<FaceCullColumn> vCols(static_cast<std::size_t>(nU * nR));
    for (int lr = lr0; lr < lr1; ++lr) for (int lu = lu0; lu < lu1; ++lu)
        vCols[static_cast<std::size_t>((lu - lu0) + nU * (lr - lr0))] =
            packCachedColumn(chunk, lu, 0, lr, 1, -kFaceCullHalo, kColBits);
    for (int lv = lv0; lv < lv1; ++lv) {
        const int bit = faceCullBitIndex(lv);
        for (CellFace face : {CellFace::VPos, CellFace::VNeg}) {
            std::fill(mask.begin(), mask.end(), CachedGreedyMaskCell{});
            for (int lr = lr0; lr < lr1; ++lr) for (int lu = lu0; lu < lu1; ++lu) {
                const auto& col = vCols[static_cast<std::size_t>((lu - lu0) + nU * (lr - lr0))];
                const FaceCullWord exposed = (face == CellFace::VPos)
                    ? cullMacroPos(col.solid, col.refined)
                    : cullMacroNeg(col.solid, col.refined);
                if (!faceCullTest(exposed, bit)) continue;
                mask[static_cast<std::size_t>(lu + S * lr)] = {chunk.getLocal(lu, lv, lr), true};
            }
            cachedGreedyMask(mask, S, S, [&](int gu, int gr, int w, int h, BlockType type) {
                const auto a = cachedWorldAddress(chunk, gu, lv, gr);
                emitCachedMacroFaceExtent(builder, chunk, gu, lv, gr, a, face, type, w, h);
            });
        }
    }

    for(int lv=lv0;lv<lv1;++lv) for(int lu=lu0;lu<lu1;++lu) for(int lr=lr0;lr<lr1;++lr) {
        const SurfaceCellAddress a=cachedWorldAddress(chunk,lu,lv,lr);
        if(chunk.hasMicroDetail(a)) continue;
        const BlockType type=chunk.getLocal(lu,lv,lr);
        if(!blockProperties(type).solid) continue;
        for(const CellFace face:faces) {
            int du=0,dv=0,dr=0; faceDelta(face,du,dv,dr);
            const auto neighbor=cachedWorldAddress(chunk,lu+du,lv+dv,lr+dr);
            if(chunk.hasMicroDetail(neighbor)) emitCachedTiledBoundary(builder,chunk,lu,lv,lr,a,face,type);
        }
    }
    emitCachedCrossBrickMicro(builder,chunk,lu0,lv0,lr0,lu1,lv1,lr1);
    return builder.finish();
}


CpuMeshData buildPlanetSurfaceFieldChunkMesh(const PlanetSurfaceSnapshot& planet,
                                             const PlanetChunkAddress& chunk,
                                             int step,
                                             bool addTransitionSkirts,
                                             float skirtDepth,
                                             int influencedStep) {
    Builder builder;
    if (chunk.radial != 0 || chunk.u < 0 || chunk.u >= PlanetSurface::ChunksPerFaceAxis ||
        chunk.v < 0 || chunk.v >= PlanetSurface::ChunksPerFaceAxis) return builder.finish();

    step = std::clamp(step,1,PlanetSurface::ChunkSize);
    const int u0 = chunk.u * PlanetSurface::ChunkSize;
    const int v0 = chunk.v * PlanetSurface::ChunkSize;
    const int u1 = std::min(u0 + PlanetSurface::ChunkSize, PlanetSurfaceSnapshot::FaceResolution);
    const int v1 = std::min(v0 + PlanetSurface::ChunkSize, PlanetSurfaceSnapshot::FaceResolution);

    auto surfacePoint = [&](int uEdge,int vEdge) {
        const Vec3 d = faceGridCornerDirection(chunk.face,uEdge,vEdge,PlanetSurfaceSnapshot::FaceResolution);
        const FaceUv owner = directionToFaceUv(d);
        const float fu = (owner.u + 1.0f) * 0.5f * static_cast<float>(PlanetSurfaceSnapshot::FaceResolution);
        const float fv = (owner.v + 1.0f) * 0.5f * static_cast<float>(PlanetSurfaceSnapshot::FaceResolution);
        const int u = std::clamp(static_cast<int>(std::floor(fu)),0,PlanetSurfaceSnapshot::FaceResolution-1);
        const int v = std::clamp(static_cast<int>(std::floor(fv)),0,PlanetSurfaceSnapshot::FaceResolution-1);
        const int r = std::max(0,planet.surfaceRadial(owner.face,u,v));
        const float radius = planet.referenceRadius + static_cast<float>(r + 1 - PlanetSurfaceSnapshot::ReferenceRadial);
        return d * radius;
    };

    influencedStep=std::clamp(influencedStep,1,step);
    const auto chunkInfluence=planet.editInfluence(chunk);
    const int significantInfluencedStep=(chunkInfluence.significanceScore()>=24)?1:influencedStep;
    auto emitPatch=[&](int u,int v,int ue,int ve) {
        const int su = std::min(u + (ue-u)/2, PlanetSurfaceSnapshot::FaceResolution-1);
        const int sv = std::min(v + (ve-v)/2, PlanetSurfaceSnapshot::FaceResolution-1);
        const int sr = planet.surfaceRadial(chunk.face,su,sv);
        if (sr < 0) return;
        BlockType material = planet.get(chunk.face,su,sv,sr);
        if (!blockProperties(material).solid) material = BlockType::Stone;

        Vec3 p0=surfacePoint(u,v);
        Vec3 p1=surfacePoint(ue,v);
        Vec3 p2=surfacePoint(ue,ve);
        Vec3 p3=surfacePoint(u,ve);
        const Vec3 desired = normalize(avg4(p0,p1,p2,p3));
        // Material tag preserves batching; Color4u tint makes Temperate field LOD
        // readable by height/biome/slope instead of a flat grass wash.
        builder.emitColored(material, fieldSurfaceColor(planet,chunk.face,su,sv,sr,material),
                            p0,p1,p2,p3,desired,false);
    };

    for (int v=v0; v<v1; v+=step) {
        const int ve = std::min(v+step,v1);
        for (int u=u0; u<u1; u+=step) {
            const int ue = std::min(u+step,u1);
            // Player edits refine only the affected coarse field tile instead
            // of promoting an entire 32x32 chunk. This preserves towers, pits,
            // and other authored silhouette changes while keeping unrelated
            // far-field area at the cheaper sampling rate.
            if(step>significantInfluencedStep && planet.hasEditInfluence(chunk.face,u,v,ue,ve)) {
                for(int sv=v;sv<ve;sv+=significantInfluencedStep) {
                    const int sve=std::min(sv+significantInfluencedStep,ve);
                    for(int su=u;su<ue;su+=significantInfluencedStep)
                        emitPatch(su,sv,std::min(su+significantInfluencedStep,ue),sve);
                }
            } else {
                emitPatch(u,v,ue,ve);
            }
        }
    }

    // Conservative transition skirts hide cracks between independently sampled
    // field tiers and between a field proxy and an adjacent full-detail chunk.
    // They deliberately trade a little overdraw for topology continuity.
    if (addTransitionSkirts && skirtDepth>0.0f) {
        auto emitSkirt=[&](Vec3 a,Vec3 b) {
            const Vec3 ai=a-normalize(a)*skirtDepth;
            const Vec3 bi=b-normalize(b)*skirtDepth;
            const Vec3 desired=normalize(cross(b-a,ai-a));
            builder.emit(BlockType::Stone,a,b,bi,ai,desired,0.68f,false);
        };
        for (int u=u0;u<u1;u+=step) {
            const int ue=std::min(u+step,u1);
            emitSkirt(surfacePoint(u,v0),surfacePoint(ue,v0));
            emitSkirt(surfacePoint(ue,v1),surfacePoint(u,v1));
        }
        for (int v=v0;v<v1;v+=step) {
            const int ve=std::min(v+step,v1);
            emitSkirt(surfacePoint(u0,ve),surfacePoint(u0,v));
            emitSkirt(surfacePoint(u1,v),surfacePoint(u1,ve));
        }
    }
    return builder.finish();
}


CpuMeshData buildPlanetOrbitalClimateShellMesh(const PlanetSurfaceSnapshot& planet,
                                               int step,
                                               float shellOffset) {
    Builder builder;
    step=std::clamp(step,1,PlanetSurfaceSnapshot::FaceResolution);
    for (int f=0;f<PlanetSurfaceSnapshot::FaceCount;++f) {
        const auto face=static_cast<CubeFace>(f);
        auto point=[&](int uEdge,int vEdge) {
            const Vec3 d=faceGridCornerDirection(face,uEdge,vEdge,PlanetSurfaceSnapshot::FaceResolution);
            const FaceUv owner=directionToFaceUv(d);
            const float fu=(owner.u+1.0f)*0.5f*PlanetSurfaceSnapshot::FaceResolution;
            const float fv=(owner.v+1.0f)*0.5f*PlanetSurfaceSnapshot::FaceResolution;
            const int u=std::clamp(static_cast<int>(std::floor(fu)),0,PlanetSurfaceSnapshot::FaceResolution-1);
            const int v=std::clamp(static_cast<int>(std::floor(fv)),0,PlanetSurfaceSnapshot::FaceResolution-1);
            const int r=generatedSurfaceAt(planet,owner.face,u,v);
            const float radius=planet.referenceRadius + static_cast<float>(r+1-PlanetSurfaceSnapshot::ReferenceRadial) + shellOffset;
            return d*radius;
        };
        for (int v=0;v<PlanetSurfaceSnapshot::FaceResolution;v+=step) {
            const int ve=std::min(v+step,PlanetSurfaceSnapshot::FaceResolution);
            for (int u=0;u<PlanetSurfaceSnapshot::FaceResolution;u+=step) {
                const int ue=std::min(u+step,PlanetSurfaceSnapshot::FaceResolution);
                const int su=std::min(u+(ue-u)/2,PlanetSurfaceSnapshot::FaceResolution-1);
                const int sv=std::min(v+(ve-v)/2,PlanetSurfaceSnapshot::FaceResolution-1);
                const int sr=generatedSurfaceAt(planet,face,su,sv);
                const Color4u c=climateColor(planet,face,su,sv,sr);
                Vec3 p0=point(u,v),p1=point(ue,v),p2=point(ue,ve),p3=point(u,ve);
                builder.emitColored(BlockType::Stone,c,p0,p1,p2,p3,normalize(avg4(p0,p1,p2,p3)));
            }
        }
    }
    return builder.finish();
}

CpuMeshData buildPlanetOrbitalCloudShellMesh(const PlanetSurfaceSnapshot& planet,
                                             int step,
                                             float cloudAltitude) {
    Builder builder;
    step=std::clamp(step,2,PlanetSurfaceSnapshot::FaceResolution);
    const float radius=planet.referenceRadius + cloudAltitude;
    for (int f=0;f<PlanetSurfaceSnapshot::FaceCount;++f) {
        const auto face=static_cast<CubeFace>(f);
        for (int v=0;v<PlanetSurfaceSnapshot::FaceResolution;v+=step) {
            const int ve=std::min(v+step,PlanetSurfaceSnapshot::FaceResolution);
            for (int u=0;u<PlanetSurfaceSnapshot::FaceResolution;u+=step) {
                const int ue=std::min(u+step,PlanetSurfaceSnapshot::FaceResolution);
                const float n=hash01(planet.seed,u/step,v/step,f,0xC10D5ULL);
                const float threshold=planet.planetClass==PlanetClass::Barren?0.92f:0.58f;
                if (n<threshold) continue;
                auto q=[&](int x,int y){return faceGridCornerDirection(face,x,y,PlanetSurfaceSnapshot::FaceResolution)*radius;};
                Vec3 p0=q(u,v),p1=q(ue,v),p2=q(ue,ve),p3=q(u,ve);
                const std::uint8_t alpha=static_cast<std::uint8_t>(std::clamp(90 + static_cast<int>((n-threshold)*280.0f),90,190));
                Color4u c=planet.planetClass==PlanetClass::Scorched?Color4u{174,133,119,alpha}:Color4u{215,224,226,alpha};
                builder.emitColored(BlockType::Stone,c,p0,p1,p2,p3,normalize(avg4(p0,p1,p2,p3)));
            }
        }
    }
    return builder.finish();
}

CpuMeshData buildPlanetSurfaceMesh(const PlanetSurfaceSnapshot& planet) {
    CpuMeshData combined{};
    for(int f=0;f<PlanetSurfaceSnapshot::FaceCount;++f) {
        for(int cv=0;cv<PlanetSurface::ChunksPerFaceAxis;++cv) {
            for(int cu=0;cu<PlanetSurface::ChunksPerFaceAxis;++cu) {
                CpuMeshData part = buildPlanetSurfaceChunkMesh(planet,{static_cast<CubeFace>(f),cu,cv,0});
                const int baseVertex = combined.vertexCount();
                combined.vertices.insert(combined.vertices.end(),part.vertices.begin(),part.vertices.end());
                combined.normals.insert(combined.normals.end(),part.normals.begin(),part.normals.end());
                combined.colors.insert(combined.colors.end(),part.colors.begin(),part.colors.end());
                for (auto range : part.materialRanges) {
                    range.firstVertex += baseVertex;
                    combined.materialRanges.push_back(range);
                }
                combined.quads += part.quads;
                combined.macroQuads += part.macroQuads;
                combined.microQuads += part.microQuads;
                combined.aoDarkenedCorners += part.aoDarkenedCorners;
            }
        }
    }
    return combined;
}

} // namespace elysium
