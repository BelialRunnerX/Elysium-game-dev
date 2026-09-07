#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace elysium {

// Campaign / system standing for Imperial attention. One slot of standing;
// use SuspicionLedger for per-system isolation. Intentionally free of
// entt::entity and dense planet-column maps. Industry/mining emit activity
// into raise(); SurfaceSiegeDirector only reads value().
class SuspicionState {
public:
    static constexpr float kMax = 100.0f;
    static constexpr float kDefaultDecayPerSecond = 0.06f;

    float value() const { return value_; }
    float claimFloor() const { return claimFloor_; }

    // Structural claim floor. Raising the floor also lifts standing up to it.
    void setClaimFloor(float floor);

    // Save/load and explicit restores. Does not auto-apply claimFloor.
    void setValue(float value);

    void raise(float amount);
    void decay(float dt, float ratePerSecond = kDefaultDecayPerSecond);
    void reduceTowardFloor(float amount);

private:
    float value_{};
    float claimFloor_{};
};

// Sparse Empire standing keyed by stable system id (not planet index, not
// entt::entity). Alpha vertical slice typically owns one system (galaxy seed);
// multi-system isolation is required by Part 24 / CP8.
class SuspicionLedger {
public:
    // Ensure and return standing for systemId. Default-constructs on first touch.
    SuspicionState& system(std::uint64_t systemId);
    const SuspicionState* find(std::uint64_t systemId) const;

    float valueOrZero(std::uint64_t systemId) const;
    float claimFloorOrZero(std::uint64_t systemId) const;

    void decayAll(float dt, float ratePerSecond = SuspicionState::kDefaultDecayPerSecond);

    std::size_t systemCount() const { return systems_.size(); }
    bool empty() const { return systems_.empty(); }
    void clear() { systems_.clear(); }

private:
    std::unordered_map<std::uint64_t, SuspicionState> systems_;
};

} // namespace elysium
