#pragma once

#include "core/Math.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace elysium {

class ChunkVoxelSpans {
public:
    enum class Mode : std::uint8_t {
        Homogeneous = 0,
        RleColumns = 1,
        Dense = 2
    };

    struct Run {
        std::uint8_t length{};
        BlockType type{BlockType::Air};
    };

    static constexpr float DenseByteRatio = 0.85f;

    ChunkVoxelSpans() = default;
    explicit ChunkVoxelSpans(int extent, BlockType fill = BlockType::Air);

    static ChunkVoxelSpans encodeAdaptive(const BlockType* cells, int extent);
    static ChunkVoxelSpans fromDense(std::vector<BlockType> cells, int extent);

    Mode mode() const { return mode_; }
    int extent() const { return extent_; }
    BlockType homogeneousValue() const { return homogeneous_; }
    std::size_t runCount() const { return runs_.size(); }
    std::size_t columnCount() const {
        return static_cast<std::size_t>(extent_) * static_cast<std::size_t>(extent_);
    }

    BlockType get(int u, int v, int radial) const;
    std::vector<BlockType> materialize() const;
    std::size_t estimatedBytes() const;

    static int flatIndex(int u, int v, int radial, int extent) {
        return radial + extent * (u + extent * v);
    }

private:
    Mode mode_{Mode::Homogeneous};
    int extent_{0};
    BlockType homogeneous_{BlockType::Air};
    std::vector<Run> runs_;
    std::vector<std::uint32_t> columnRunOffsets_;
    std::vector<BlockType> dense_;

    void encodeFromDense(const BlockType* cells);
};

struct SphericalNoiseBasis {
    static constexpr int OctaveCount = 4;
    std::array<Vec3, OctaveCount> axes{};
    std::array<float, OctaveCount> phases{};
    std::array<float, OctaveCount> frequencies{2.4f, 5.1f, 10.7f, 19.3f};
    std::array<float, OctaveCount> amplitudes{1.0f, 0.52f, 0.25f, 0.12f};

    static SphericalNoiseBasis make(std::uint64_t seed, std::uint64_t fieldLabel);
    float evaluate(Vec3 direction) const;
};

} // namespace elysium
