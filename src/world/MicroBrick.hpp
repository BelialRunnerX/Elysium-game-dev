#pragma once

#include "world/Block.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace elysium {

class MicroBrick {
public:
    static constexpr int Resolution = 16;
    static constexpr int CellCount = Resolution * Resolution * Resolution;
    static constexpr int DensePromotionThreshold = 512;

    enum class StorageMode : std::uint8_t { Sparse, Dense };

    explicit MicroBrick(BlockType baseline = BlockType::Air) : baseline_(baseline) {}

    BlockType baseline() const { return baseline_; }
    StorageMode storageMode() const { return dense_.has_value() ? StorageMode::Dense : StorageMode::Sparse; }
    std::size_t overrideCount() const;

    BlockType get(int x, int y, int z) const;
    void set(int x, int y, int z, BlockType type);
    BlockType getIndex(int index) const;
    void setIndex(int index, BlockType type);

    std::vector<std::pair<std::uint16_t, BlockType>> overrides() const;

    static int index(int x, int y, int z) { return x + Resolution * (z + Resolution * y); }
    static bool inBounds(int x, int y, int z) {
        return x >= 0 && x < Resolution && y >= 0 && y < Resolution && z >= 0 && z < Resolution;
    }

private:
    BlockType baseline_{BlockType::Air};
    std::vector<std::pair<std::uint16_t, BlockType>> sparse_; // sorted by index
    std::optional<std::array<BlockType, CellCount>> dense_;

    void promoteDense();
};

} // namespace elysium
