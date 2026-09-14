#include "world/VoxelMesher.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace elysium {
namespace {

Color4u shade(Color4u c, float s) {
    c.r = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.r) * s), 0, 255));
    c.g = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.g) * s), 0, 255));
    c.b = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.b) * s), 0, 255));
    return c;
}

float faceShade(int nx, int ny, int nz) {
    if (ny > 0) return 1.00f;
    if (ny < 0) return 0.58f;
    if (nx > 0) return 0.88f;
    if (nx < 0) return 0.73f;
    if (nz > 0) return 0.82f;
    return 0.68f;
}

struct MeshBucket {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<std::uint8_t> colors;
    int quads{};
    int macroQuads{};
    int microQuads{};
    int aoDarkenedCorners{};
};

class MeshBuilder {
public:
    void emit(BlockType type, Vec3 normal,
              Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
              const std::array<Color4u,4>& cornerColors,
              bool micro, int darkenedCorners) {
        const auto bucketIndex = static_cast<std::size_t>(type);
        if (bucketIndex >= buckets_.size()) return;
        auto& b = buckets_[bucketIndex];
        const std::array<Vec3, 6> verts{p0,p1,p2,p0,p2,p3};
        constexpr std::array<int,6> ci{0,1,2,0,2,3};
        for (std::size_t i=0;i<verts.size();++i) {
            const Vec3 p=verts[i];
            const Color4u c=cornerColors[static_cast<std::size_t>(ci[i])];
            b.vertices.push_back(p.x); b.vertices.push_back(p.y); b.vertices.push_back(p.z);
            b.normals.push_back(normal.x); b.normals.push_back(normal.y); b.normals.push_back(normal.z);
            b.colors.push_back(c.r); b.colors.push_back(c.g); b.colors.push_back(c.b); b.colors.push_back(c.a);
        }
        ++b.quads;
        if (micro) ++b.microQuads; else ++b.macroQuads;
        b.aoDarkenedCorners += darkenedCorners;
    }

    CpuMeshData finish() {
        CpuMeshData out;
        for (int raw=1; raw<kBlockTypeCount; ++raw) {
            auto& b=buckets_[static_cast<std::size_t>(raw)];
            if (b.vertices.empty()) continue;
            MaterialRange range{};
            range.material=static_cast<BlockType>(raw);
            range.firstVertex=out.vertexCount();
            range.vertexCount=static_cast<int>(b.vertices.size()/3U);
            range.quads=b.quads;
            out.materialRanges.push_back(range);
            out.vertices.insert(out.vertices.end(),b.vertices.begin(),b.vertices.end());
            out.normals.insert(out.normals.end(),b.normals.begin(),b.normals.end());
            out.colors.insert(out.colors.end(),b.colors.begin(),b.colors.end());
            out.quads += b.quads;
            out.macroQuads += b.macroQuads;
            out.microQuads += b.microQuads;
            out.aoDarkenedCorners += b.aoDarkenedCorners;
        }
        return out;
    }

private:
    std::array<MeshBucket,kBlockTypeCount> buckets_{};
};

struct MaskCell {
    BlockType type{BlockType::Air};
    bool active{};
};

// Generic 2D rectangle merger used by each face plane.
template <class Emit>
void greedyMask(std::vector<MaskCell>& mask, int width, int height, Emit&& emit) {
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

bool macroFaceCandidate(const WorldSnapshot& world, int x, int y, int z, int nx, int ny, int nz) {
    if (!world.inBounds(x,y,z) || world.isRefined(x,y,z)) return false;
    const BlockType type = world.get(x,y,z);
    if (!blockProperties(type).solid) return false;
    const int qx = x + nx, qy = y + ny, qz = z + nz;
    if (!world.inBounds(qx,qy,qz)) return true;
    if (world.isRefined(qx,qy,qz)) return false; // emitted at micro boundary resolution
    if (blockProperties(world.get(qx,qy,qz)).solid) return false;
    return world.isExteriorAir(qx,qy,qz);
}

int floorDiv(int value, int divisor) {
    int q = value / divisor;
    const int r = value % divisor;
    if (r < 0) --q;
    return q;
}

int floorMod(int value, int divisor) {
    int r = value % divisor;
    if (r < 0) r += divisor;
    return r;
}

BlockType microGlobalGet(const WorldSnapshot& world, int gx, int gy, int gz) {
    const int r = MicroBrick::Resolution;
    const int x = floorDiv(gx,r), y = floorDiv(gy,r), z = floorDiv(gz,r);
    if (!world.inBounds(x,y,z)) return BlockType::Air;
    return world.microGet(x,y,z, floorMod(gx,r), floorMod(gy,r), floorMod(gz,r));
}

int dominantAxis(Vec3 n) {
    if (std::abs(n.x) > 0.5f) return 0;
    if (std::abs(n.y) > 0.5f) return 1;
    return 2;
}

float component(Vec3 v,int axis) {
    return axis==0?v.x:(axis==1?v.y:v.z);
}

void setComponent(IVec3& v,int axis,int value) {
    if(axis==0) v.x=value; else if(axis==1) v.y=value; else v.z=value;
}

int component(IVec3 v,int axis) {
    return axis==0?v.x:(axis==1?v.y:v.z);
}

// Four cells in the outside layer that touch a face vertex are sampled. This is
// a stable, inexpensive voxel AO approximation: flat terrain remains bright,
// while wall/floor intersections and chipped micro cavities darken locally.
template <class SolidFn>
float vertexAo(IVec3 p, Vec3 normal, SolidFn&& solid, bool& darkened) {
    const int n=dominantAxis(normal);
    const int a=(n+1)%3;
    const int b=(n+2)%3;
    IVec3 q{};
    setComponent(q,n, component(normal,n)>0.0f ? component(p,n) : component(p,n)-1);
    int occupied=0;
    for (int da : {-1,0}) for (int db : {-1,0}) {
        setComponent(q,a,component(p,a)+da);
        setComponent(q,b,component(p,b)+db);
        if (solid(q.x,q.y,q.z)) ++occupied;
    }
    darkened = occupied>0;
    return std::clamp(1.0f - 0.11f*static_cast<float>(occupied),0.56f,1.0f);
}

void emitMacroQuad(MeshBuilder& builder,const WorldSnapshot& world,BlockType type,Vec3 normal,
                   Vec3 p0,Vec3 p1,Vec3 p2,Vec3 p3) {
    const std::array<Vec3,4> p{p0,p1,p2,p3};
    std::array<Color4u,4> colors{};
    int dark=0;
    const float base=faceShade(static_cast<int>(normal.x),static_cast<int>(normal.y),static_cast<int>(normal.z));
    for (std::size_t i=0;i<4;++i) {
        IVec3 ip{static_cast<int>(std::lround(p[i].x)),static_cast<int>(std::lround(p[i].y)),static_cast<int>(std::lround(p[i].z))};
        bool d=false;
        const float ao=vertexAo(ip,normal,[&](int x,int y,int z){ return blockProperties(world.get(x,y,z)).solid; },d);
        if(d) ++dark;
        colors[i]=shade(blockProperties(type).color,base*ao);
    }
    builder.emit(type,normal,p0,p1,p2,p3,colors,false,dark);
}

void emitMicroQuad(MeshBuilder& builder,const WorldSnapshot& world,BlockType type,Vec3 normal,
                   IVec3 p0,IVec3 p1,IVec3 p2,IVec3 p3) {
    constexpr float s=1.0f/static_cast<float>(MicroBrick::Resolution);
    const std::array<IVec3,4> ip{p0,p1,p2,p3};
    std::array<Color4u,4> colors{};
    int dark=0;
    const float base=faceShade(static_cast<int>(normal.x),static_cast<int>(normal.y),static_cast<int>(normal.z));
    for(std::size_t i=0;i<4;++i) {
        bool d=false;
        const float ao=vertexAo(ip[i],normal,[&](int x,int y,int z){ return blockProperties(microGlobalGet(world,x,y,z)).solid; },d);
        if(d) ++dark;
        colors[i]=shade(blockProperties(type).color,base*ao);
    }
    auto cv=[&](IVec3 q){ return Vec3{q.x*s,q.y*s,q.z*s}; };
    builder.emit(type,normal,cv(p0),cv(p1),cv(p2),cv(p3),colors,true,dark);
}

void emitMicroFace(MeshBuilder& builder,const WorldSnapshot& world,int gx,int gy,int gz,int nx,int ny,int nz,BlockType type) {
    const int x0=gx,y0=gy,z0=gz,x1=gx+1,y1=gy+1,z1=gz+1;
    if (nx > 0) emitMicroQuad(builder,world,type,{1,0,0},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1});
    else if (nx < 0) emitMicroQuad(builder,world,type,{-1,0,0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{x0,y0,z0});
    else if (ny > 0) emitMicroQuad(builder,world,type,{0,1,0},{x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0});
    else if (ny < 0) emitMicroQuad(builder,world,type,{0,-1,0},{x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1});
    else if (nz > 0) emitMicroQuad(builder,world,type,{0,0,1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{x0,y0,z1});
    else emitMicroQuad(builder,world,type,{0,0,-1},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{x1,y0,z0});
}

void emitRefinedCells(MeshBuilder& builder, const WorldSnapshot& world,
                      int x0, int y0, int z0, int x1, int y1, int z1) {
    constexpr int r = MicroBrick::Resolution;
    constexpr IVec3 dirs[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int y = y0; y < y1; ++y) for (int z = z0; z < z1; ++z) for (int x = x0; x < x1; ++x) {
        const auto* brick = world.microBrick(x,y,z);
        if (!brick) continue;
        const int baseGX = x*r, baseGY = y*r, baseGZ = z*r;
        for (int my = 0; my < r; ++my) for (int mz = 0; mz < r; ++mz) for (int mx = 0; mx < r; ++mx) {
            const BlockType type = brick->get(mx,my,mz);
            if (!blockProperties(type).solid) continue;
            const int gx=baseGX+mx, gy=baseGY+my, gz=baseGZ+mz;
            for (const auto& d : dirs) {
                if (!blockProperties(microGlobalGet(world,gx+d.x,gy+d.y,gz+d.z)).solid)
                    emitMicroFace(builder,world,gx,gy,gz,d.x,d.y,d.z,type);
            }
        }
    }
}

// When an unrefined macro voxel borders a refined cell, the normal greedy face
// cannot represent a 6.25 cm hole. Emit only the exposed sub-quads on that boundary.
void emitMacroToRefinedBoundaries(MeshBuilder& builder, const WorldSnapshot& world,
                                  int x0, int y0, int z0, int x1, int y1, int z1) {
    constexpr int r = MicroBrick::Resolution;
    constexpr IVec3 dirs[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int y=y0;y<y1;++y) for (int z=z0;z<z1;++z) for (int x=x0;x<x1;++x) {
        if (world.isRefined(x,y,z)) continue;
        const BlockType type = world.get(x,y,z);
        if (!blockProperties(type).solid) continue;
        for (const auto& d : dirs) {
            const int qx=x+d.x,qy=y+d.y,qz=z+d.z;
            const auto* neighbor = world.microBrick(qx,qy,qz);
            if (!neighbor) continue;
            for (int b=0;b<r;++b) for (int a=0;a<r;++a) {
                int nmx=0,nmy=0,nmz=0;
                int gx=0,gy=0,gz=0;
                if (d.x > 0) { nmx=0; nmy=b; nmz=a; gx=x*r+r-1; gy=y*r+b; gz=z*r+a; }
                else if (d.x < 0) { nmx=r-1; nmy=b; nmz=a; gx=x*r; gy=y*r+b; gz=z*r+a; }
                else if (d.y > 0) { nmy=0; nmx=a; nmz=b; gx=x*r+a; gy=y*r+r-1; gz=z*r+b; }
                else if (d.y < 0) { nmy=r-1; nmx=a; nmz=b; gx=x*r+a; gy=y*r; gz=z*r+b; }
                else if (d.z > 0) { nmz=0; nmx=a; nmy=b; gx=x*r+a; gy=y*r+b; gz=z*r+r-1; }
                else { nmz=r-1; nmx=a; nmy=b; gx=x*r+a; gy=y*r+b; gz=z*r; }
                if (!blockProperties(neighbor->get(nmx,nmy,nmz)).solid)
                    emitMicroFace(builder,world,gx,gy,gz,d.x,d.y,d.z,type);
            }
        }
    }
}

} // namespace

CpuMeshData buildChunkMesh(const WorldSnapshot& world, int chunkX, int chunkY, int chunkZ) {
    MeshBuilder builder;
    const int x0 = chunkX * WorldSnapshot::ChunkSize;
    const int y0 = chunkY * WorldSnapshot::ChunkSize;
    const int z0 = chunkZ * WorldSnapshot::ChunkSize;
    const int x1 = std::min(x0 + WorldSnapshot::ChunkSize, WorldSnapshot::Width);
    const int y1 = std::min(y0 + WorldSnapshot::ChunkSize, WorldSnapshot::Height);
    const int z1 = std::min(z0 + WorldSnapshot::ChunkSize, WorldSnapshot::Depth);
    if (x0 >= x1 || y0 >= y1 || z0 >= z1) return builder.finish();

    // +/- X faces: mask axes are Z (u) and Y (v).
    std::vector<MaskCell> mask(static_cast<std::size_t>((z1-z0)*(y1-y0)));
    for (int x=x0;x<x1;++x) {
        for (int sign : {1,-1}) {
            std::fill(mask.begin(),mask.end(),MaskCell{});
            for (int y=y0;y<y1;++y) for (int z=z0;z<z1;++z) {
                if (macroFaceCandidate(world,x,y,z,sign,0,0))
                    mask[static_cast<std::size_t>((z-z0)+(z1-z0)*(y-y0))] = {world.get(x,y,z),true};
            }
            greedyMask(mask,z1-z0,y1-y0,[&](int u,int v,int w,int h,BlockType type){
                const float px = static_cast<float>(x + (sign>0 ? 1 : 0));
                const float ya=static_cast<float>(y0+v), yb=static_cast<float>(y0+v+h);
                const float za=static_cast<float>(z0+u), zb=static_cast<float>(z0+u+w);
                if(sign>0) emitMacroQuad(builder,world,type,{1,0,0},{px,ya,za},{px,yb,za},{px,yb,zb},{px,ya,zb});
                else emitMacroQuad(builder,world,type,{-1,0,0},{px,ya,zb},{px,yb,zb},{px,yb,za},{px,ya,za});
            });
        }
    }

    // +/- Y faces: mask axes are X (u) and Z (v).
    mask.assign(static_cast<std::size_t>((x1-x0)*(z1-z0)),{});
    for (int y=y0;y<y1;++y) {
        for (int sign : {1,-1}) {
            std::fill(mask.begin(),mask.end(),MaskCell{});
            for (int z=z0;z<z1;++z) for (int x=x0;x<x1;++x) {
                if (macroFaceCandidate(world,x,y,z,0,sign,0))
                    mask[static_cast<std::size_t>((x-x0)+(x1-x0)*(z-z0))] = {world.get(x,y,z),true};
            }
            greedyMask(mask,x1-x0,z1-z0,[&](int u,int v,int w,int h,BlockType type){
                const float py=static_cast<float>(y+(sign>0?1:0));
                const float xa=static_cast<float>(x0+u), xb=static_cast<float>(x0+u+w);
                const float za=static_cast<float>(z0+v), zb=static_cast<float>(z0+v+h);
                if(sign>0) emitMacroQuad(builder,world,type,{0,1,0},{xa,py,zb},{xb,py,zb},{xb,py,za},{xa,py,za});
                else emitMacroQuad(builder,world,type,{0,-1,0},{xa,py,za},{xb,py,za},{xb,py,zb},{xa,py,zb});
            });
        }
    }

    // +/- Z faces: mask axes are X (u) and Y (v).
    mask.assign(static_cast<std::size_t>((x1-x0)*(y1-y0)),{});
    for (int z=z0;z<z1;++z) {
        for (int sign : {1,-1}) {
            std::fill(mask.begin(),mask.end(),MaskCell{});
            for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
                if (macroFaceCandidate(world,x,y,z,0,0,sign))
                    mask[static_cast<std::size_t>((x-x0)+(x1-x0)*(y-y0))] = {world.get(x,y,z),true};
            }
            greedyMask(mask,x1-x0,y1-y0,[&](int u,int v,int w,int h,BlockType type){
                const float pz=static_cast<float>(z+(sign>0?1:0));
                const float xa=static_cast<float>(x0+u), xb=static_cast<float>(x0+u+w);
                const float ya=static_cast<float>(y0+v), yb=static_cast<float>(y0+v+h);
                if(sign>0) emitMacroQuad(builder,world,type,{0,0,1},{xb,ya,pz},{xb,yb,pz},{xa,yb,pz},{xa,ya,pz});
                else emitMacroQuad(builder,world,type,{0,0,-1},{xa,ya,pz},{xa,yb,pz},{xb,yb,pz},{xb,ya,pz});
            });
        }
    }

    emitMacroToRefinedBoundaries(builder,world,x0,y0,z0,x1,y1,z1);
    emitRefinedCells(builder,world,x0,y0,z0,x1,y1,z1);
    return builder.finish();
}

} // namespace elysium
