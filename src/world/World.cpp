#include "world/World.hpp"

#include "core/Determinism.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace elysium {
namespace {
float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise2D(std::uint64_t seed, float x, float z, int scale, std::uint64_t label) {
    const float fx = x / static_cast<float>(scale);
    const float fz = z / static_cast<float>(scale);
    const int x0 = static_cast<int>(std::floor(fx));
    const int z0 = static_cast<int>(std::floor(fz));
    const int x1 = x0 + 1;
    const int z1 = z0 + 1;
    const float tx = smoothstep(fx - static_cast<float>(x0));
    const float tz = smoothstep(fz - static_cast<float>(z0));

    const float a = hash01(seed, x0, 0, z0, label);
    const float b = hash01(seed, x1, 0, z0, label);
    const float c = hash01(seed, x0, 0, z1, label);
    const float d = hash01(seed, x1, 0, z1, label);
    const float ab = a + (b - a) * tx;
    const float cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
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
}

bool WorldSnapshot::inBounds(int x, int y, int z) const {
    return x >= 0 && x < Width && y >= 0 && y < Height && z >= 0 && z < Depth;
}

int WorldSnapshot::flatIndex(int x, int y, int z) const {
    return x + Width * (z + Depth * y);
}

BlockType WorldSnapshot::get(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return BlockType::Air;
    return blocks[static_cast<std::size_t>(flatIndex(x,y,z))];
}

bool WorldSnapshot::isRefined(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return false;
    return microBricks.find(flatIndex(x,y,z)) != microBricks.end();
}

const MicroBrick* WorldSnapshot::microBrick(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return nullptr;
    const auto it = microBricks.find(flatIndex(x,y,z));
    return it == microBricks.end() ? nullptr : &it->second;
}

BlockType WorldSnapshot::microGet(int x, int y, int z, int mx, int my, int mz) const {
    if (!inBounds(x,y,z)) return BlockType::Air;
    if (const auto* brick = microBrick(x,y,z)) return brick->get(mx,my,mz);
    return get(x,y,z);
}

bool WorldSnapshot::isExteriorAir(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return true;
    if (exteriorAir.empty()) return get(x,y,z) == BlockType::Air;
    return exteriorAir[static_cast<std::size_t>(flatIndex(x,y,z))] != 0;
}

void WorldSnapshot::computeExteriorAir() {
    exteriorAir.assign(static_cast<std::size_t>(Width * Height * Depth), 0);
    std::deque<IVec3> open;

    auto push = [&](int x, int y, int z) {
        if (!inBounds(x,y,z)) return;
        const int idx = flatIndex(x,y,z);
        auto& mark = exteriorAir[static_cast<std::size_t>(idx)];
        if (mark != 0) return;
        if (get(x,y,z) != BlockType::Air) return;
        // Refined cells are treated as occupied at macro flood-fill resolution.
        if (isRefined(x,y,z)) return;
        mark = 1;
        open.push_back({x,y,z});
    };

    for (int x = 0; x < Width; ++x) for (int z = 0; z < Depth; ++z) {
        push(x,0,z); push(x,Height-1,z);
    }
    for (int y = 0; y < Height; ++y) for (int z = 0; z < Depth; ++z) {
        push(0,y,z); push(Width-1,y,z);
    }
    for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x) {
        push(x,y,0); push(x,y,Depth-1);
    }

    constexpr IVec3 dirs[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    while (!open.empty()) {
        const IVec3 c = open.front();
        open.pop_front();
        for (const auto& d : dirs) push(c.x+d.x, c.y+d.y, c.z+d.z);
    }
}

World::World(std::uint64_t seed, PlanetClass planetClass)
    : seed_(seed), class_(planetClass),
      baseline_(Width * Height * Depth, BlockType::Air),
      blocks_(Width * Height * Depth, BlockType::Air) {
    generate();
    exteriorAirCache_ = computeExteriorAirMask();
    chunkRevisions_.fill(revision_);
    rebuildAllChunkOccupancy();
}

const PlanetEnvironment& World::environment() const {
    return planetEnvironmentFor(class_);
}

bool World::inBounds(int x, int y, int z) const {
    return x >= 0 && x < Width && y >= 0 && y < Height && z >= 0 && z < Depth;
}

int World::flatIndex(int x, int y, int z) const {
    return x + Width * (z + Depth * y);
}

IVec3 World::cellFromFlatIndex(int index) const {
    const int y = index / (Width * Depth);
    const int rem = index - y * Width * Depth;
    const int z = rem / Width;
    const int x = rem - z * Width;
    return {x, y, z};
}

BlockType World::get(int x, int y, int z) const {
    if (!inBounds(x, y, z)) return BlockType::Air;
    return blocks_[static_cast<std::size_t>(flatIndex(x, y, z))];
}

BlockType World::baseline(int x, int y, int z) const {
    if (!inBounds(x, y, z)) return BlockType::Air;
    return baseline_[static_cast<std::size_t>(flatIndex(x, y, z))];
}

bool World::isSolid(int x, int y, int z) const {
    if (y < 0) return true;
    if (x < 0 || x >= Width || z < 0 || z >= Depth) return true;
    if (y >= Height) return false;
    // Collision remains macro-resolution in this pass. The refinement layer is
    // authoritative for rendering/persistence and ready for finer collision later.
    return blockProperties(get(x, y, z)).solid;
}

void World::set(int x, int y, int z, BlockType type) {
    if (!inBounds(x, y, z)) return;
    const int idx = flatIndex(x, y, z);
    auto& current = blocks_[static_cast<std::size_t>(idx)];
    const bool hadRefinement = microBricks_.find(idx) != microBricks_.end();
    if (current == type && !hadRefinement) return;

    const bool oldSolid = blockProperties(current).solid;
    const bool newSolid = blockProperties(type).solid;
    beginEdit();
    current = type;
    microBricks_.erase(idx); // macro replacement supersedes sculpted state
    if (type == baseline_[static_cast<std::size_t>(idx)]) edits_.erase(idx);
    else edits_[idx] = type;
    markCellAndSeamNeighborsDirty(x,y,z);
    noteCellOccupancyEdit(x, y, z, oldSolid, newSolid, type);
    if (oldSolid != newSolid) refreshExteriorConnectivityAndDirty();
}

void World::applySavedEdit(int idx, BlockType type) {
    if (idx < 0 || idx >= static_cast<int>(blocks_.size())) return;
    const IVec3 c = cellFromFlatIndex(idx);
    const bool oldSolid = blockProperties(blocks_[static_cast<std::size_t>(idx)]).solid;
    const bool newSolid = blockProperties(type).solid;
    beginEdit();
    blocks_[static_cast<std::size_t>(idx)] = type;
    microBricks_.erase(idx);
    if (type == baseline_[static_cast<std::size_t>(idx)]) edits_.erase(idx);
    else edits_[idx] = type;
    markCellAndSeamNeighborsDirty(c.x,c.y,c.z);
    noteCellOccupancyEdit(c.x, c.y, c.z, oldSolid, newSolid, type);
    if (oldSolid != newSolid) refreshExteriorConnectivityAndDirty();
}

bool World::isRefined(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return false;
    return microBricks_.find(flatIndex(x,y,z)) != microBricks_.end();
}

const MicroBrick* World::microBrick(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return nullptr;
    const auto it = microBricks_.find(flatIndex(x,y,z));
    return it == microBricks_.end() ? nullptr : &it->second;
}

BlockType World::microGet(int x, int y, int z, int mx, int my, int mz) const {
    if (!inBounds(x,y,z)) return BlockType::Air;
    if (const auto* brick = microBrick(x,y,z)) return brick->get(mx,my,mz);
    return get(x,y,z);
}

MicroBrick& World::refineCell(int x, int y, int z) {
    const int idx = flatIndex(x,y,z);
    auto [it, inserted] = microBricks_.try_emplace(idx, get(x,y,z));
    (void)inserted;
    return it->second;
}

void World::setMicro(int x, int y, int z, int mx, int my, int mz, BlockType type) {
    if (!inBounds(x,y,z) || !MicroBrick::inBounds(mx,my,mz)) return;
    const int idx = flatIndex(x,y,z);
    auto& brick = refineCell(x,y,z);
    const BlockType old = brick.get(mx,my,mz);
    if (old == type) return;
    const bool wasOcc = blockProperties(old).solid; // micro cell
    // Macro occupancy for extents: cell occupied if any micro solid OR (unrefined) macro solid.
    auto cellOccupied = [&](const MicroBrick* b, BlockType macro) {
        if (!b) return blockProperties(macro).solid;
        for (int i = 0; i < MicroBrick::CellCount; ++i)
            if (blockProperties(b->getIndex(i)).solid) return true;
        return false;
    };
    const bool wasCell = cellOccupied(&brick, get(x,y,z));
    beginEdit();
    brick.set(mx,my,mz,type);
    if (brick.overrideCount() == 0) microBricks_.erase(idx);
    const MicroBrick* after = microBrick(x,y,z);
    const bool nowCell = cellOccupied(after, get(x,y,z));
    markCellAndSeamNeighborsDirty(x,y,z);
    noteCellOccupancyEdit(x, y, z, wasCell, nowCell, nowCell ? BlockType::Stone : BlockType::Air);
    (void)wasOcc;
}

void World::setMicroGlobal(int gx, int gy, int gz, BlockType type) {
    const int x = floorDiv(gx, MicroBrick::Resolution);
    const int y = floorDiv(gy, MicroBrick::Resolution);
    const int z = floorDiv(gz, MicroBrick::Resolution);
    if (!inBounds(x,y,z)) return;
    setMicro(x,y,z,
             floorMod(gx,MicroBrick::Resolution),
             floorMod(gy,MicroBrick::Resolution),
             floorMod(gz,MicroBrick::Resolution), type);
}

void World::applySavedMicroEdit(int flatCellIndex, int microIndex, BlockType type) {
    if (flatCellIndex < 0 || flatCellIndex >= static_cast<int>(blocks_.size())) return;
    if (microIndex < 0 || microIndex >= MicroBrick::CellCount) return;
    const IVec3 c = cellFromFlatIndex(flatCellIndex);
    auto& brick = refineCell(c.x,c.y,c.z);
    const BlockType old = brick.getIndex(microIndex);
    if (old == type) return;
    auto cellOccupied = [&](const MicroBrick* b, BlockType macro) {
        if (!b) return blockProperties(macro).solid;
        for (int i = 0; i < MicroBrick::CellCount; ++i)
            if (blockProperties(b->getIndex(i)).solid) return true;
        return false;
    };
    const bool wasCell = cellOccupied(&brick, blocks_[static_cast<std::size_t>(flatCellIndex)]);
    beginEdit();
    brick.setIndex(microIndex,type);
    const bool nowCell = cellOccupied(&brick, blocks_[static_cast<std::size_t>(flatCellIndex)]);
    markCellAndSeamNeighborsDirty(c.x,c.y,c.z);
    noteCellOccupancyEdit(c.x, c.y, c.z, wasCell, nowCell, nowCell ? BlockType::Stone : BlockType::Air);
}

std::size_t World::microOverrideCount() const {
    std::size_t total = 0;
    for (const auto& [_, brick] : microBricks_) total += brick.overrideCount();
    return total;
}

int World::generatedHeight(int x, int z) const {
    const float n1 = valueNoise2D(seed_, static_cast<float>(x), static_cast<float>(z), 22, 0x4C41594FULL);
    const float n2 = valueNoise2D(seed_, static_cast<float>(x), static_cast<float>(z), 9, 0x5249444745ULL);
    const float n = n1 * 0.72f + n2 * 0.28f;

    float base = 8.0f;
    float amplitude = 4.0f;
    // Temperate keeps default base/amplitude. Catalog::flates get distinct
    // profiles; Barren/Scorched values are unchanged (no GeneratorVersion bump).
    if (class_ == PlanetClass::Barren) { base = 7.0f; amplitude = 3.5f; }
    else if (class_ == PlanetClass::Scorched) { base = 7.0f; amplitude = 5.0f; }
    else if (class_ == PlanetClass::Frozen) { base = 7.5f; amplitude = 3.0f; }
    else if (class_ == PlanetClass::Toxic) { base = 7.5f; amplitude = 3.8f; }
    else if (class_ == PlanetClass::Irradiated) { base = 7.0f; amplitude = 4.2f; }
    else if (class_ == PlanetClass::Oceanic) { base = 6.5f; amplitude = 2.8f; }
    else if (class_ == PlanetClass::Anomalous) { base = 8.0f; amplitude = 5.5f; }

    int h = static_cast<int>(std::round(base + (n - 0.5f) * 2.0f * amplitude));
    const int cx = Width / 2;
    const int cz = Depth / 2;
    const int md = std::abs(x - cx) + std::abs(z - cz);
    if (md < 7) h = static_cast<int>(std::round(base));
    return std::clamp(h, 3, Height - 7);
}

int World::surfaceY(int x, int z) const {
    if (x < 0 || x >= Width || z < 0 || z >= Depth) return 0;
    for (int y = Height - 1; y >= 0; --y) {
        if (blockProperties(get(x, y, z)).solid) return y;
    }
    return 0;
}

void World::generate() {
    for (int z = 0; z < Depth; ++z) {
        for (int x = 0; x < Width; ++x) {
            const int h = generatedHeight(x, z);
            for (int y = 0; y < Height; ++y) {
                BlockType type = BlockType::Air;
                if (y <= h) {
                    // Class columns for Temperate/Barren/Scorched are bit-identical
                    // to the prior baseline (GeneratorVersion unchanged).
                    if (class_ == PlanetClass::Temperate) {
                        type = (y == h) ? BlockType::Grass : ((y >= h - 2) ? BlockType::Dirt : BlockType::Stone);
                    } else if (class_ == PlanetClass::Barren) {
                        type = (y >= h - 2) ? BlockType::Regolith : BlockType::Stone;
                    } else if (class_ == PlanetClass::Scorched) {
                        type = (y >= h - 2) ? BlockType::Basalt : BlockType::Stone;
                    } else if (class_ == PlanetClass::Frozen) {
                        type = (y >= h - 2) ? BlockType::Stone : BlockType::Stone;
                    } else if (class_ == PlanetClass::Toxic) {
                        type = (y == h) ? BlockType::Dirt : ((y >= h - 2) ? BlockType::Dirt : BlockType::Stone);
                    } else if (class_ == PlanetClass::Irradiated) {
                        type = (y >= h - 2) ? BlockType::Regolith : BlockType::Stone;
                    } else if (class_ == PlanetClass::Oceanic) {
                        type = (y == h) ? BlockType::Grass : ((y >= h - 2) ? BlockType::Dirt : BlockType::Stone);
                    } else { // Anomalous (+ Count fallback)
                        type = (y == h) ? BlockType::Stone : ((y >= h - 2) ? BlockType::Dirt : BlockType::Stone);
                    }

                    if (y < h - 2) {
                        const float ore = hash01(seed_, x, y, z, 0x4F524553ULL);
                        if (ore < 0.012f && y < 9) type = BlockType::IronOre;
                        else if (ore < 0.026f && y < 12) type = BlockType::TinOre;
                        else if (ore < 0.048f && y < 14) type = BlockType::CopperOre;
                        else if (ore < 0.078f && y < 17) type = BlockType::CoalOre;
                    }

                    if (class_ == PlanetClass::Scorched && y <= 2) {
                        const float magma = hash01(seed_, x, y, z, 0x4D41474D41ULL);
                        if (magma < 0.18f) type = BlockType::Magma;
                    }
                }
                const auto idx = static_cast<std::size_t>(flatIndex(x, y, z));
                baseline_[idx] = type;
                blocks_[idx] = type;
            }
        }
    }
}

std::optional<VoxelHit> World::raycast(Vec3 origin, Vec3 direction, float maxDistance) const {
    direction = normalize(direction);
    if (lengthSq(direction) < 0.5f) return std::nullopt;

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    const int stepX = direction.x > 0 ? 1 : -1;
    const int stepY = direction.y > 0 ? 1 : -1;
    const int stepZ = direction.z > 0 ? 1 : -1;

    const float inf = std::numeric_limits<float>::infinity();
    const float tDeltaX = std::abs(direction.x) > 1e-7f ? std::abs(1.0f / direction.x) : inf;
    const float tDeltaY = std::abs(direction.y) > 1e-7f ? std::abs(1.0f / direction.y) : inf;
    const float tDeltaZ = std::abs(direction.z) > 1e-7f ? std::abs(1.0f / direction.z) : inf;

    auto firstT = [](float o, int cell, int step, float dir) {
        if (std::abs(dir) <= 1e-7f) return std::numeric_limits<float>::infinity();
        const float boundary = static_cast<float>(cell + (step > 0 ? 1 : 0));
        return (boundary - o) / dir;
    };

    float tMaxX = firstT(origin.x, x, stepX, direction.x);
    float tMaxY = firstT(origin.y, y, stepY, direction.y);
    float tMaxZ = firstT(origin.z, z, stepZ, direction.z);
    IVec3 lastNormal{};
    float t = 0.0f;

    for (int i = 0; i < 256 && t <= maxDistance; ++i) {
        if (inBounds(x, y, z)) {
            const BlockType type = get(x, y, z);
            if (type != BlockType::Air) return VoxelHit{{x,y,z}, lastNormal, t, type};
        }

        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX; t = tMaxX; tMaxX += tDeltaX; lastNormal = {-stepX,0,0};
        } else if (tMaxY < tMaxZ) {
            y += stepY; t = tMaxY; tMaxY += tDeltaY; lastNormal = {0,-stepY,0};
        } else {
            z += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; lastNormal = {0,0,-stepZ};
        }
    }
    return std::nullopt;
}

std::optional<MicroVoxelHit> World::raycastMicro(Vec3 origin, Vec3 direction, float maxDistance) const {
    direction = normalize(direction);
    if (lengthSq(direction) < 0.5f) return std::nullopt;

    constexpr float scale = static_cast<float>(MicroBrick::Resolution);
    const Vec3 o = origin * scale;
    int gx = static_cast<int>(std::floor(o.x));
    int gy = static_cast<int>(std::floor(o.y));
    int gz = static_cast<int>(std::floor(o.z));

    const int stepX = direction.x > 0 ? 1 : -1;
    const int stepY = direction.y > 0 ? 1 : -1;
    const int stepZ = direction.z > 0 ? 1 : -1;
    const float inf = std::numeric_limits<float>::infinity();
    const float tDeltaX = std::abs(direction.x) > 1e-7f ? std::abs(1.0f / direction.x) : inf;
    const float tDeltaY = std::abs(direction.y) > 1e-7f ? std::abs(1.0f / direction.y) : inf;
    const float tDeltaZ = std::abs(direction.z) > 1e-7f ? std::abs(1.0f / direction.z) : inf;
    auto firstT = [](float value, int cell, int step, float dir) {
        if (std::abs(dir) <= 1e-7f) return std::numeric_limits<float>::infinity();
        const float boundary = static_cast<float>(cell + (step > 0 ? 1 : 0));
        return (boundary - value) / dir;
    };

    float tMaxX = firstT(o.x, gx, stepX, direction.x);
    float tMaxY = firstT(o.y, gy, stepY, direction.y);
    float tMaxZ = firstT(o.z, gz, stepZ, direction.z);
    IVec3 lastNormal{};
    float t = 0.0f;
    const float maxT = maxDistance * scale;

    for (int i = 0; i < 4096 && t <= maxT; ++i) {
        const int x = floorDiv(gx, MicroBrick::Resolution);
        const int y = floorDiv(gy, MicroBrick::Resolution);
        const int z = floorDiv(gz, MicroBrick::Resolution);
        if (inBounds(x,y,z)) {
            const int mx = floorMod(gx,MicroBrick::Resolution);
            const int my = floorMod(gy,MicroBrick::Resolution);
            const int mz = floorMod(gz,MicroBrick::Resolution);
            const BlockType type = microGet(x,y,z,mx,my,mz);
            if (type != BlockType::Air) {
                return MicroVoxelHit{{x,y,z},{mx,my,mz},{gx,gy,gz},lastNormal,t/scale,type};
            }
        }

        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            gx += stepX; t = tMaxX; tMaxX += tDeltaX; lastNormal = {-stepX,0,0};
        } else if (tMaxY < tMaxZ) {
            gy += stepY; t = tMaxY; tMaxY += tDeltaY; lastNormal = {0,-stepY,0};
        } else {
            gz += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; lastNormal = {0,0,-stepZ};
        }
    }
    return std::nullopt;
}

float World::localHazardAt(Vec3 pos) const {
    float hazard = environment().hazardDamagePerSecond;
    const int cx = static_cast<int>(std::floor(pos.x));
    const int cy = static_cast<int>(std::floor(pos.y));
    const int cz = static_cast<int>(std::floor(pos.z));
    for (int y = cy - 2; y <= cy + 1; ++y) {
        for (int z = cz - 2; z <= cz + 2; ++z) {
            for (int x = cx - 2; x <= cx + 2; ++x) {
                if (get(x,y,z) == BlockType::Magma) hazard += 2.0f;
            }
        }
    }
    return hazard;
}


SealedVolumeQuery World::sealedVolume(IVec3 start, int maxCells) const {
    SealedVolumeQuery result{};
    if (maxCells <= 0 || !inBounds(start.x,start.y,start.z) || isSolid(start.x,start.y,start.z)) return result;
    std::vector<std::uint8_t> visited(static_cast<std::size_t>(Width*Height*Depth),0);
    std::deque<IVec3> open;
    const int startIdx=flatIndex(start.x,start.y,start.z);
    visited[static_cast<std::size_t>(startIdx)]=1;
    open.push_back(start);
    result.cells.reserve(static_cast<std::size_t>(std::min(maxCells,1024)));
    bool touchesExterior=false;
    constexpr IVec3 dirs[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

    while(!open.empty()) {
        const IVec3 c=open.front(); open.pop_front();
        result.cells.push_back(flatIndex(c.x,c.y,c.z));
        if(static_cast<int>(result.cells.size())>maxCells) {
            result.truncated=true;
            result.sealed=false;
            return result;
        }
        if(c.x==0 || c.x==Width-1 || c.y==0 || c.y==Height-1 || c.z==0 || c.z==Depth-1) touchesExterior=true;
        for(const auto& d:dirs) {
            const int nx=c.x+d.x,ny=c.y+d.y,nz=c.z+d.z;
            if(!inBounds(nx,ny,nz)) { touchesExterior=true; continue; }
            if(isSolid(nx,ny,nz)) continue;
            const int idx=flatIndex(nx,ny,nz);
            auto& mark=visited[static_cast<std::size_t>(idx)];
            if(mark) continue;
            mark=1;
            open.push_back({nx,ny,nz});
        }
    }
    result.sealed=!touchesExterior;
    return result;
}

WorldSnapshot World::snapshot() const {
    WorldSnapshot result{};
    result.revision = revision_;
    result.blocks = blocks_;
    result.microBricks = microBricks_;
    result.exteriorAir = exteriorAirCache_;
    return result;
}


int World::chunkIndex(int chunkX, int chunkY, int chunkZ) {
    if (chunkX < 0 || chunkX >= ChunkCountX ||
        chunkY < 0 || chunkY >= ChunkCountY ||
        chunkZ < 0 || chunkZ >= ChunkCountZ) return -1;
    return chunkX + ChunkCountX * (chunkZ + ChunkCountZ * chunkY);
}

std::uint64_t World::chunkRevision(int chunkX, int chunkY, int chunkZ) const {
    const int idx = chunkIndex(chunkX,chunkY,chunkZ);
    if (idx < 0) return 0;
    return chunkRevisions_[static_cast<std::size_t>(idx)];
}

int World::dirtyChunkCountSince(std::uint64_t revision) const {
    int count = 0;
    for (const auto r : chunkRevisions_) if (r > revision) ++count;
    return count;
}


const ChunkAdaptiveSummary& World::chunkOccupancy(int chunkX, int chunkY, int chunkZ) const {
    const int idx = chunkIndex(chunkX, chunkY, chunkZ);
    static const ChunkAdaptiveSummary kEmpty{};
    if (idx < 0) return kEmpty;
    return chunkOccupancy_[static_cast<std::size_t>(idx)];
}

void World::rebuildChunkOccupancy(int chunkX, int chunkY, int chunkZ) {
    const int idx = chunkIndex(chunkX, chunkY, chunkZ);
    if (idx < 0) return;
    const int x0 = chunkX * ChunkSize;
    const int y0 = chunkY * ChunkSize;
    const int z0 = chunkZ * ChunkSize;
    const int x1 = std::min(x0 + ChunkSize, Width);
    const int y1 = std::min(y0 + ChunkSize, Height);
    const int z1 = std::min(z0 + ChunkSize, Depth);
    const int sizeX = x1 - x0;
    const int sizeY = y1 - y0;
    const int sizeZ = z1 - z0;
    chunkOccupancy_[static_cast<std::size_t>(idx)] = analyzeChunkOccupancy(sizeX, sizeY, sizeZ,
        [&](int lx, int ly, int lz) {
            const int x = x0 + lx, y = y0 + ly, z = z0 + lz;
            if (isRefined(x, y, z)) {
                const auto* brick = microBrick(x, y, z);
                if (!brick) return BlockType::Air;
                for (int i = 0; i < MicroBrick::CellCount; ++i)
                    if (blockProperties(brick->getIndex(i)).solid) return BlockType::Stone;
                return BlockType::Air;
            }
            return get(x, y, z);
        });
}

void World::fillChunk(int chunkX, int chunkY, int chunkZ, BlockType type) {
    const int idx = chunkIndex(chunkX, chunkY, chunkZ);
    if (idx < 0) return;
    const int x0 = chunkX * ChunkSize;
    const int y0 = chunkY * ChunkSize;
    const int z0 = chunkZ * ChunkSize;
    const int x1 = std::min(x0 + ChunkSize, Width);
    const int y1 = std::min(y0 + ChunkSize, Height);
    const int z1 = std::min(z0 + ChunkSize, Depth);
    beginEdit();
    bool anySolidChange = false;
    for (int z = z0; z < z1; ++z)
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const int flat = flatIndex(x, y, z);
                auto& current = blocks_[static_cast<std::size_t>(flat)];
                const bool hadRefinement = microBricks_.find(flat) != microBricks_.end();
                if (current == type && !hadRefinement) continue;
                const bool oldSolid = blockProperties(current).solid;
                const bool newSolid = blockProperties(type).solid;
                current = type;
                microBricks_.erase(flat);
                if (type == baseline_[static_cast<std::size_t>(flat)]) edits_.erase(flat);
                else edits_[flat] = type;
                markCellAndSeamNeighborsDirty(x, y, z);
                if (oldSolid != newSolid) anySolidChange = true;
            }
    rebuildChunkOccupancy(chunkX, chunkY, chunkZ);
    if (anySolidChange) refreshExteriorConnectivityAndDirty();
}

void World::rebuildAllChunkOccupancy() {
    for (int cz = 0; cz < ChunkCountZ; ++cz)
        for (int cy = 0; cy < ChunkCountY; ++cy)
            for (int cx = 0; cx < ChunkCountX; ++cx)
                rebuildChunkOccupancy(cx, cy, cz);
}

void World::noteCellOccupancyEdit(int x, int y, int z, bool wasSolid, bool nowSolid, BlockType nowType) {
    if (!inBounds(x, y, z)) return;
    const int cx = x / ChunkSize, cy = y / ChunkSize, cz = z / ChunkSize;
    const int idx = chunkIndex(cx, cy, cz);
    if (idx < 0) return;
    auto& summary = chunkOccupancy_[static_cast<std::size_t>(idx)];
    const int lx = x - cx * ChunkSize;
    const int ly = y - cy * ChunkSize;
    const int lz = z - cz * ChunkSize;
    const int sizeX = std::min(ChunkSize, Width - cx * ChunkSize);
    const int sizeY = std::min(ChunkSize, Height - cy * ChunkSize);
    const int sizeZ = std::min(ChunkSize, Depth - cz * ChunkSize);
    bool needsRescan = false;
    applyChunkSolidEdit(summary, lx, ly, lz, wasSolid, nowSolid, nowType, sizeX, sizeY, sizeZ, needsRescan);
    if (needsRescan) rebuildChunkOccupancy(cx, cy, cz);
}

void World::beginEdit() {
    ++revision_;
}

void World::markChunkDirty(int chunkX, int chunkY, int chunkZ) {
    const int idx = chunkIndex(chunkX,chunkY,chunkZ);
    if (idx < 0) return;
    chunkRevisions_[static_cast<std::size_t>(idx)] = revision_;
}

void World::markCellAndSeamNeighborsDirty(int x, int y, int z) {
    if (!inBounds(x,y,z)) return;
    const int cx=x/ChunkSize, cy=y/ChunkSize, cz=z/ChunkSize;
    markChunkDirty(cx,cy,cz);
    if (x % ChunkSize == 0) markChunkDirty(cx-1,cy,cz);
    if (x % ChunkSize == ChunkSize-1) markChunkDirty(cx+1,cy,cz);
    if (y % ChunkSize == 0) markChunkDirty(cx,cy-1,cz);
    if (y % ChunkSize == ChunkSize-1) markChunkDirty(cx,cy+1,cz);
    if (z % ChunkSize == 0) markChunkDirty(cx,cy,cz-1);
    if (z % ChunkSize == ChunkSize-1) markChunkDirty(cx,cy,cz+1);
}

std::vector<std::uint8_t> World::computeExteriorAirMask() const {
    WorldSnapshot temp{};
    temp.revision = revision_;
    temp.blocks = blocks_;
    temp.microBricks = microBricks_;
    temp.computeExteriorAir();
    return std::move(temp.exteriorAir);
}

void World::refreshExteriorConnectivityAndDirty() {
    std::vector<std::uint8_t> next = computeExteriorAirMask();
    if (exteriorAirCache_.size() != next.size()) {
        exteriorAirCache_ = std::move(next);
        for (int cy=0;cy<ChunkCountY;++cy)
            for (int cz=0;cz<ChunkCountZ;++cz)
                for (int cx=0;cx<ChunkCountX;++cx)
                    markChunkDirty(cx,cy,cz);
        return;
    }

    constexpr IVec3 dirs[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int idx=0; idx<static_cast<int>(next.size()); ++idx) {
        if (next[static_cast<std::size_t>(idx)] == exteriorAirCache_[static_cast<std::size_t>(idx)]) continue;
        const IVec3 c = cellFromFlatIndex(idx);
        markCellAndSeamNeighborsDirty(c.x,c.y,c.z);
        for (const auto& d : dirs) markCellAndSeamNeighborsDirty(c.x+d.x,c.y+d.y,c.z+d.z);
    }
    exteriorAirCache_ = std::move(next);
}

} // namespace elysium
