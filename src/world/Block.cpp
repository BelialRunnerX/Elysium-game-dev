#include "world/Block.hpp"

#include <array>
#include <stdexcept>

namespace elysium {
namespace {
constexpr std::array<BlockProperties, kBlockTypeCount> kBlocks{{
    {"Air",             0.0f, 0, false, false, false, false, 0.0f, {0,0,0,0}},
    {"Grass",           0.5f, 0, true,  true,  true,  false, 2.0f, {93,132,75,255}},
    {"Dirt",            0.6f, 0, true,  true,  true,  false, 2.0f, {111,82,55,255}},
    {"Stone",           2.4f, 1, true,  true,  true,  false, 2.0f, {113,118,126,255}},
    {"Regolith",        0.8f, 0, true,  true,  true,  false, 2.0f, {145,133,120,255}},
    {"Basalt",          3.6f, 2, true,  true,  true,  false, 2.0f, {69,67,73,255}},
    {"Coal Ore",        1.8f, 1, true,  true,  true,  true,  4.0f, {57,58,61,255}},
    {"Copper Ore",      2.1f, 1, true,  true,  true,  true,  4.0f, {154,92,63,255}},
    {"Tin Ore",         1.9f, 1, true,  true,  true,  true,  4.0f, {151,158,162,255}},
    {"Iron Ore",        2.6f, 2, true,  true,  true,  true,  4.0f, {142,128,117,255}},
    {"Planks",          1.6f, 0, true,  true,  true,  false, 0.0f, {151,111,67,255}},
    {"Steel Plate",     6.0f, 3, true,  true,  true,  false, 0.0f, {101,116,128,255}},
    {"Registry Beacon", 8.0f, 3, true,  true,  true,  false, 0.0f, {24,90,70,255}},
    {"Magma",         999.0f, 9, true,  false, false, false, 0.0f, {226,82,35,255}},
    {"Door Panel",       3.5f, 2, true,  true,  true,  false, 0.0f, {91,105,111,255}},
    {"Airlock Panel",    5.0f, 3, true,  true,  true,  false, 0.0f, {48,86,80,255}},
}};
}

const BlockProperties& blockProperties(BlockType type) {
    const auto i = static_cast<std::size_t>(type);
    if (i >= kBlocks.size()) throw std::out_of_range("invalid block type");
    return kBlocks[i];
}

BlockType miningDrop(BlockType type) {
    if (type == BlockType::Grass) return BlockType::Dirt;
    return type;
}

} // namespace elysium
