#pragma once

#include <array>
#include <cstdint>

namespace elysium {

// Placement domain for a filed Registry Beacon. Block-only today; stable IDs
// remain portable when the beacon later becomes a machine/ECS object.
enum class RegistryBeaconDomain : std::uint8_t {
    None = 0,
    Planar = 1,
    Spherical = 2
};

// Per-planet claim objective slot. `claimed` is the territorial filing flag;
// `beaconIntact` is the live objective probe used by SurfaceSiegeDirector.
// They are deliberately distinct so a Register Action can fail on beacon loss
// without conflating the two arguments at the Game call site.
struct RegistryBeaconSlot {
    bool claimed{false};
    bool beaconIntact{false};
    std::uint64_t stableBeaconId{};
    RegistryBeaconDomain domain{RegistryBeaconDomain::None};
};

// World-layer claim/beacon authority (no EnTT, no MachineType, no raylib).
// Does not renumber MachineTypes — beacon remains BlockType::RegistryBeacon
// until a later append-only machine migration.
class RegistryBeaconAuthority {
public:
    static constexpr int kPlanetCount = 3;

    explicit RegistryBeaconAuthority(std::uint64_t worldSeed = 0);

    static bool validPlanetIndex(int planetIndex);

    const RegistryBeaconSlot& slot(int planetIndex) const;
    bool claimed(int planetIndex) const;
    bool beaconIntact(int planetIndex) const;
    std::uint64_t stableBeaconId(int planetIndex) const;
    RegistryBeaconDomain domain(int planetIndex) const;

    // Place/file a Registry Beacon: sets claimed + beaconIntact and allocates a
    // non-zero stable identity. Re-filing an already-intact claim keeps the
    // existing stable ID (idempotent for the live objective).
    // Returns the stable beacon ID, or 0 on invalid planet.
    std::uint64_t fileClaim(int planetIndex, RegistryBeaconDomain domain);

    // Beacon destroyed: clears intact + claim (structures/edits remain the
    // world's responsibility). Returns the prior stable ID if the beacon was
    // intact, else 0.
    std::uint64_t destroyBeacon(int planetIndex);

    // Schema-8 / legacy restore: only claimed bools exist on disk. Migrates
    // preexisting filed claims to intact objectives with allocated stable IDs
    // so director probes and future machine migration see live slots.
    void restoreClaimedFlags(bool c0, bool c1, bool c2);

    // Explicit slot restore for tests / future embeds.
    bool restoreSlot(int planetIndex, const RegistryBeaconSlot& slot);

private:
    std::uint64_t worldSeed_{};
    std::uint64_t nextSerial_{1};
    std::array<RegistryBeaconSlot, kPlanetCount> slots_{};

    std::uint64_t allocateStableId();
    void migrateFiledSlot(int planetIndex);
};

} // namespace elysium
