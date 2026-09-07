#include "world/ChunkVoxelSpans.hpp"

#include "core/Determinism.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace elysium {
namespace {

constexpr float kTau = 6.28318530717958647692f;

float seededUnit(std::uint64_t seed, std::uint64_t label, int channel) {
    return hash01(seed, channel, static_cast<int>(label & 0x7fffffffULL),
                  static_cast<int>((label >> 31U) & 0x7fffffffULL), 0x53504852ULL);
}

Vec3 seededAxis(std::uint64_t seed, std::uint64_t label, int channel) {
    Vec3 a{
        seededUnit(seed, label, channel * 3 + 0) * 2.0f - 1.0f,
        seededUnit(seed, label, channel * 3 + 1) * 2.0f - 1.0f,
        seededUnit(seed, label, channel * 3 + 2) * 2.0f - 1.0f
    };
    if (lengthSq(a) < 0.05f) a = {0.31f, 0.71f, 0.43f};
    return normalize(a);
}

} // namespace

ChunkVoxelSpans::ChunkVoxelSpans(int extent, BlockType fill)
    : mode_(Mode::Homogeneous), extent_(extent), homogeneous_(fill) {
    if (extent_ <= 0 || extent_ > 255) {
        throw std::invalid_argument("ChunkVoxelSpans extent out of range");
    }
}

ChunkVoxelSpans ChunkVoxelSpans::encodeAdaptive(const BlockType* cells, int extent) {
    if (!cells || extent <= 0 || extent > 255) {
        throw std::invalid_argument("ChunkVoxelSpans encodeAdaptive requires valid cells/extent");
    }
    ChunkVoxelSpans out;
    out.extent_ = extent;
    out.encodeFromDense(cells);
    return out;
}

ChunkVoxelSpans ChunkVoxelSpans::fromDense(std::vector<BlockType> cells, int extent) {
    const std::size_t expected =
        static_cast<std::size_t>(extent) * static_cast<std::size_t>(extent) *
        static_cast<std::size_t>(extent);
    if (cells.size() != expected) throw std::invalid_argument("ChunkVoxelSpans fromDense size mismatch");
    ChunkVoxelSpans out;
    out.mode_ = Mode::Dense;
    out.extent_ = extent;
    out.dense_ = std::move(cells);
    return out;
}

void ChunkVoxelSpans::encodeFromDense(const BlockType* cells) {
    const int e = extent_;
    const std::size_t total =
        static_cast<std::size_t>(e) * static_cast<std::size_t>(e) * static_cast<std::size_t>(e);
    const BlockType first = cells[0];
    bool homogeneous = true;
    for (std::size_t i = 1; i < total; ++i) {
        if (cells[i] != first) {
            homogeneous = false;
            break;
        }
    }
    if (homogeneous) {
        mode_ = Mode::Homogeneous;
        homogeneous_ = first;
        runs_.clear();
        columnRunOffsets_.clear();
        dense_.clear();
        return;
    }

    runs_.clear();
    columnRunOffsets_.clear();
    columnRunOffsets_.reserve(static_cast<std::size_t>(e) * static_cast<std::size_t>(e) + 1U);
    columnRunOffsets_.push_back(0);
    for (int v = 0; v < e; ++v) {
        for (int u = 0; u < e; ++u) {
            int radial = 0;
            while (radial < e) {
                const BlockType type = cells[static_cast<std::size_t>(flatIndex(u, v, radial, e))];
                int runLen = 1;
                while (radial + runLen < e &&
                       cells[static_cast<std::size_t>(flatIndex(u, v, radial + runLen, e))] == type) {
                    ++runLen;
                }
                int remaining = runLen;
                while (remaining > 0) {
                    const int piece = std::min(remaining, 255);
                    runs_.push_back(Run{static_cast<std::uint8_t>(piece), type});
                    remaining -= piece;
                }
                radial += runLen;
            }
            columnRunOffsets_.push_back(static_cast<std::uint32_t>(runs_.size()));
        }
    }

    const std::size_t denseBytes = total * sizeof(BlockType);
    const std::size_t rleBytes =
        runs_.size() * sizeof(Run) + columnRunOffsets_.size() * sizeof(std::uint32_t);
    if (static_cast<float>(rleBytes) >= DenseByteRatio * static_cast<float>(denseBytes)) {
        mode_ = Mode::Dense;
        dense_.assign(cells, cells + static_cast<std::ptrdiff_t>(total));
        runs_.clear();
        runs_.shrink_to_fit();
        columnRunOffsets_.clear();
        columnRunOffsets_.shrink_to_fit();
        return;
    }

    mode_ = Mode::RleColumns;
    dense_.clear();
    dense_.shrink_to_fit();
}

BlockType ChunkVoxelSpans::get(int u, int v, int radial) const {
    if (u < 0 || v < 0 || radial < 0 || u >= extent_ || v >= extent_ || radial >= extent_) {
        return BlockType::Air;
    }
    switch (mode_) {
        case Mode::Homogeneous:
            return homogeneous_;
        case Mode::Dense: {
            const int idx = flatIndex(u, v, radial, extent_);
            return dense_[static_cast<std::size_t>(idx)];
        }
        case Mode::RleColumns: {
            const std::size_t col =
                static_cast<std::size_t>(u) +
                static_cast<std::size_t>(extent_) * static_cast<std::size_t>(v);
            const std::uint32_t begin = columnRunOffsets_[col];
            const std::uint32_t end = columnRunOffsets_[col + 1U];
            int cursor = 0;
            for (std::uint32_t i = begin; i < end; ++i) {
                const Run& run = runs_[i];
                if (radial < cursor + static_cast<int>(run.length)) return run.type;
                cursor += static_cast<int>(run.length);
            }
            return BlockType::Air;
        }
    }
    return BlockType::Air;
}

std::vector<BlockType> ChunkVoxelSpans::materialize() const {
    const std::size_t total =
        static_cast<std::size_t>(extent_) * static_cast<std::size_t>(extent_) *
        static_cast<std::size_t>(extent_);
    if (mode_ == Mode::Dense) return dense_;
    std::vector<BlockType> out(total, BlockType::Air);
    for (int v = 0; v < extent_; ++v) {
        for (int u = 0; u < extent_; ++u) {
            for (int r = 0; r < extent_; ++r) {
                out[static_cast<std::size_t>(flatIndex(u, v, r, extent_))] = get(u, v, r);
            }
        }
    }
    return out;
}

std::size_t ChunkVoxelSpans::estimatedBytes() const {
    switch (mode_) {
        case Mode::Homogeneous:
            return sizeof(BlockType);
        case Mode::Dense:
            return dense_.capacity() * sizeof(BlockType);
        case Mode::RleColumns:
            return runs_.capacity() * sizeof(Run) +
                   columnRunOffsets_.capacity() * sizeof(std::uint32_t);
    }
    return 0;
}

SphericalNoiseBasis SphericalNoiseBasis::make(std::uint64_t seed, std::uint64_t fieldLabel) {
    SphericalNoiseBasis basis{};
    for (int i = 0; i < OctaveCount; ++i) {
        basis.axes[static_cast<std::size_t>(i)] = seededAxis(seed, fieldLabel, i);
        basis.phases[static_cast<std::size_t>(i)] = seededUnit(seed, fieldLabel, 30 + i) * kTau;
    }
    return basis;
}

float SphericalNoiseBasis::evaluate(Vec3 d) const {
    float sum = 0.0f;
    float weight = 0.0f;
    for (int i = 0; i < OctaveCount; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        sum += std::sin(dot(d, axes[idx]) * frequencies[idx] * kTau + phases[idx]) * amplitudes[idx];
        weight += amplitudes[idx];
    }
    return weight > 0.0f ? sum / weight : 0.0f;
}

} // namespace elysium
