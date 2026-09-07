#include "world/RegistryBeaconClaim.hpp"

#include "core/Determinism.hpp"

namespace elysium {

RegistryBeaconAuthority::RegistryBeaconAuthority(std::uint64_t worldSeed)
    : worldSeed_(worldSeed) {}

bool RegistryBeaconAuthority::validPlanetIndex(int planetIndex) {
    return planetIndex >= 0 && planetIndex < kPlanetCount;
}

const RegistryBeaconSlot& RegistryBeaconAuthority::slot(int planetIndex) const {
    static const RegistryBeaconSlot kEmpty{};
    if (!validPlanetIndex(planetIndex)) return kEmpty;
    return slots_[static_cast<std::size_t>(planetIndex)];
}

bool RegistryBeaconAuthority::claimed(int planetIndex) const {
    return validPlanetIndex(planetIndex) &&
           slots_[static_cast<std::size_t>(planetIndex)].claimed;
}

bool RegistryBeaconAuthority::beaconIntact(int planetIndex) const {
    return validPlanetIndex(planetIndex) &&
           slots_[static_cast<std::size_t>(planetIndex)].beaconIntact;
}

std::uint64_t RegistryBeaconAuthority::stableBeaconId(int planetIndex) const {
    if (!validPlanetIndex(planetIndex)) return 0;
    return slots_[static_cast<std::size_t>(planetIndex)].stableBeaconId;
}

RegistryBeaconDomain RegistryBeaconAuthority::domain(int planetIndex) const {
    if (!validPlanetIndex(planetIndex)) return RegistryBeaconDomain::None;
    return slots_[static_cast<std::size_t>(planetIndex)].domain;
}

std::uint64_t RegistryBeaconAuthority::allocateStableId() {
    for (;;) {
        const std::uint64_t id =
            mix64(worldSeed_ ^ 0x524547424541434FULL ^ nextSerial_++); // "REGBEACO"
        if (id == 0) continue;
        bool collision = false;
        for (const auto& s : slots_) {
            if (s.stableBeaconId == id) {
                collision = true;
                break;
            }
        }
        if (!collision) return id;
    }
}

std::uint64_t RegistryBeaconAuthority::fileClaim(int planetIndex,
                                                RegistryBeaconDomain domain) {
    if (!validPlanetIndex(planetIndex) || domain == RegistryBeaconDomain::None) return 0;
    auto& s = slots_[static_cast<std::size_t>(planetIndex)];
    s.claimed = true;
    s.beaconIntact = true;
    s.domain = domain;
    if (s.stableBeaconId == 0) s.stableBeaconId = allocateStableId();
    return s.stableBeaconId;
}

std::uint64_t RegistryBeaconAuthority::destroyBeacon(int planetIndex) {
    if (!validPlanetIndex(planetIndex)) return 0;
    auto& s = slots_[static_cast<std::size_t>(planetIndex)];
    const std::uint64_t prior = s.beaconIntact ? s.stableBeaconId : 0;
    s.claimed = false;
    s.beaconIntact = false;
    s.stableBeaconId = 0;
    s.domain = RegistryBeaconDomain::None;
    return prior;
}

void RegistryBeaconAuthority::migrateFiledSlot(int planetIndex) {
    if (!validPlanetIndex(planetIndex)) return;
    auto& s = slots_[static_cast<std::size_t>(planetIndex)];
    if (!s.claimed) {
        s.beaconIntact = false;
        s.stableBeaconId = 0;
        s.domain = RegistryBeaconDomain::None;
        return;
    }
    // Preexisting filed claim (schema 8): treat as intact objective and give it
    // a stable identity so director/Game no longer alias claimed↔beaconIntact.
    s.beaconIntact = true;
    if (s.domain == RegistryBeaconDomain::None) s.domain = RegistryBeaconDomain::Spherical;
    if (s.stableBeaconId == 0) s.stableBeaconId = allocateStableId();
}

void RegistryBeaconAuthority::restoreClaimedFlags(bool c0, bool c1, bool c2) {
    slots_[0] = {};
    slots_[1] = {};
    slots_[2] = {};
    slots_[0].claimed = c0;
    slots_[1].claimed = c1;
    slots_[2].claimed = c2;
    for (int i = 0; i < kPlanetCount; ++i) migrateFiledSlot(i);
}

bool RegistryBeaconAuthority::restoreSlot(int planetIndex, const RegistryBeaconSlot& slot) {
    if (!validPlanetIndex(planetIndex)) return false;
    if (slot.claimed && slot.beaconIntact && slot.stableBeaconId == 0) return false;
    if (!slot.claimed && (slot.beaconIntact || slot.stableBeaconId != 0)) return false;
    slots_[static_cast<std::size_t>(planetIndex)] = slot;
    if (slot.claimed && !slot.beaconIntact && slot.stableBeaconId != 0) {
        // Allowed divergence: claim flag without live objective (director fails).
    }
    if (slots_[static_cast<std::size_t>(planetIndex)].claimed &&
        slots_[static_cast<std::size_t>(planetIndex)].beaconIntact &&
        slots_[static_cast<std::size_t>(planetIndex)].stableBeaconId == 0) {
        slots_[static_cast<std::size_t>(planetIndex)].stableBeaconId = allocateStableId();
    }
    return true;
}

} // namespace elysium
