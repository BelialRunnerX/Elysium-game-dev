#include "world/SurfaceSuspicion.hpp"

#include <algorithm>

namespace elysium {

void SuspicionState::setClaimFloor(float floor) {
    claimFloor_ = std::clamp(floor, 0.0f, kMax);
    if (value_ < claimFloor_) value_ = claimFloor_;
}

void SuspicionState::setValue(float value) {
    value_ = std::clamp(value, 0.0f, kMax);
}

void SuspicionState::raise(float amount) {
    if (amount <= 0.0f) return;
    value_ = std::clamp(value_ + amount, claimFloor_, kMax);
}

void SuspicionState::decay(float dt, float ratePerSecond) {
    if (dt <= 0.0f || ratePerSecond <= 0.0f) return;
    if (value_ <= claimFloor_) {
        value_ = claimFloor_;
        return;
    }
    value_ = std::max(claimFloor_, value_ - ratePerSecond * dt);
}

void SuspicionState::reduceTowardFloor(float amount) {
    if (amount <= 0.0f) return;
    value_ = std::max(claimFloor_, value_ - amount);
}

SuspicionState& SuspicionLedger::system(std::uint64_t systemId) {
    return systems_[systemId];
}

const SuspicionState* SuspicionLedger::find(std::uint64_t systemId) const {
    const auto it = systems_.find(systemId);
    if (it == systems_.end()) return nullptr;
    return &it->second;
}

float SuspicionLedger::valueOrZero(std::uint64_t systemId) const {
    if (const auto* slot = find(systemId)) return slot->value();
    return 0.0f;
}

float SuspicionLedger::claimFloorOrZero(std::uint64_t systemId) const {
    if (const auto* slot = find(systemId)) return slot->claimFloor();
    return 0.0f;
}

void SuspicionLedger::decayAll(float dt, float ratePerSecond) {
    for (auto& [id, standing] : systems_) {
        (void)id;
        standing.decay(dt, ratePerSecond);
    }
}

} // namespace elysium
