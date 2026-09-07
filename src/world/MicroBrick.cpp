#include "world/MicroBrick.hpp"

#include <algorithm>
#include <stdexcept>

namespace elysium {

std::size_t MicroBrick::overrideCount() const {
    if (!dense_) return sparse_.size();
    return static_cast<std::size_t>(std::count_if(dense_->begin(), dense_->end(),
        [this](BlockType t) { return t != baseline_; }));
}

BlockType MicroBrick::get(int x, int y, int z) const {
    if (!inBounds(x,y,z)) return BlockType::Air;
    return getIndex(index(x,y,z));
}

BlockType MicroBrick::getIndex(int idx) const {
    if (idx < 0 || idx >= CellCount) return BlockType::Air;
    if (dense_) return (*dense_)[static_cast<std::size_t>(idx)];
    const auto key = static_cast<std::uint16_t>(idx);
    const auto it = std::lower_bound(sparse_.begin(), sparse_.end(), key,
        [](const auto& p, std::uint16_t value) { return p.first < value; });
    return (it != sparse_.end() && it->first == key) ? it->second : baseline_;
}

void MicroBrick::set(int x, int y, int z, BlockType type) {
    if (!inBounds(x,y,z)) return;
    setIndex(index(x,y,z), type);
}

void MicroBrick::setIndex(int idx, BlockType type) {
    if (idx < 0 || idx >= CellCount) return;
    if (dense_) {
        (*dense_)[static_cast<std::size_t>(idx)] = type;
        return;
    }

    const auto key = static_cast<std::uint16_t>(idx);
    auto it = std::lower_bound(sparse_.begin(), sparse_.end(), key,
        [](const auto& p, std::uint16_t value) { return p.first < value; });
    if (type == baseline_) {
        if (it != sparse_.end() && it->first == key) sparse_.erase(it);
        return;
    }
    if (it != sparse_.end() && it->first == key) it->second = type;
    else sparse_.insert(it, {key, type});

    if (sparse_.size() > DensePromotionThreshold) promoteDense();
}

std::vector<std::pair<std::uint16_t, BlockType>> MicroBrick::overrides() const {
    if (!dense_) return sparse_;
    std::vector<std::pair<std::uint16_t, BlockType>> result;
    result.reserve(overrideCount());
    for (int i = 0; i < CellCount; ++i) {
        const BlockType t = (*dense_)[static_cast<std::size_t>(i)];
        if (t != baseline_) result.emplace_back(static_cast<std::uint16_t>(i), t);
    }
    return result;
}

void MicroBrick::promoteDense() {
    std::array<BlockType, CellCount> dense{};
    dense.fill(baseline_);
    for (const auto& [idx, type] : sparse_) dense[static_cast<std::size_t>(idx)] = type;
    dense_ = std::move(dense);
    sparse_.clear();
    sparse_.shrink_to_fit();
}

} // namespace elysium
