#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace elysium {

enum class BlockType : std::uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Regolith,
    Basalt,
    CoalOre,
    CopperOre,
    TinOre,
    IronOre,
    Planks,
    SteelPlate,
    RegistryBeacon,
    Magma,
    DoorPanel,
    AirlockPanel,
    Count
};

constexpr int kBlockTypeCount = static_cast<int>(BlockType::Count);

struct Color4u {
    std::uint8_t r{}, g{}, b{}, a{255};
};

struct BlockProperties {
    std::string_view name;
    float hardness;
    int harvestTier;
    bool solid;
    bool mineable;
    bool placeable;
    bool ore;
    float suspicionOnMine;
    Color4u color;
};

const BlockProperties& blockProperties(BlockType type);
BlockType miningDrop(BlockType type);

} // namespace elysium
