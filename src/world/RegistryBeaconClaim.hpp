#pragma once

#include <array>
#include <cstdint>

namespace elysium {

enum class RegistryBeaconDomain : std::uint8_t {
    None = 0,
    Planar = 1,
    Spherical = 2
};

struct RegistryBeaconSlot {
    bool claimed{false};
    bool beaconIntact{false};
    std::uint64_t stableBeaconId{};
    RegistryBeaconDomain domain{RegistryBeaconDomain::None};
};

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

    std::uint64_t fileClaim(int planetIndex, RegistryBeaconDomain domain);
    std::uint64_t destroyBeacon(int planetIndex);
    void restoreClaimedFlags(bool c0, bool c1, bool c2);
    bool restoreSlot(int planetIndex, const RegistryBeaconSlot& slot);

private:
    std::uint64_t worldSeed_{};
    std::uint64_t nextSerial_{1};
    std::array<RegistryBeaconSlot, kPlanetCount> slots_{};

    std::uint64_t allocateStableId();
    void migrateFiledSlot(int planetIndex);
};

} // namespace elysium
