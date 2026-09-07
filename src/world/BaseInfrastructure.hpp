#pragma once

#include "core/Math.hpp"
#include "world/World.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace elysium {

enum class MachineType : std::uint8_t {
    BurnerGenerator = 0,
    BatteryBank = 1,
    AtmosphereUnit = 2,
    StorageCrate = 3,
    AirlockController = 4,
    SensorMast = 5,
    Turret = 6,
    ShieldPylon = 7,
    LogicController = 8,
    // Part Two v0.19 industrial progression. Values are appended so every
    // previously persisted machine identity remains stable.
    Furnace = 9,
    AlloyCrucible = 10,
    Refinery = 11,
    NetworkStorage = 12,
    Conveyor = 13,
    Sorter = 14,
    CargoLoader = 15,
    // Part Two v0.20 expands the industrial ladder without disturbing any
    // previously persisted numeric identity.
    Crusher = 16,
    ChemicalVat = 17,
    Fabricator = 18,
    Extractor = 19
};

constexpr int machineItemId(MachineType type) {
    return 1000 + static_cast<int>(type);
}

const char* machineName(MachineType type);

struct MachineObject {
    std::uint64_t stableId{};
    MachineType type{MachineType::StorageCrate};
    IVec3 anchor{};
    bool enabled{true};
    bool powered{};
    float fuelSeconds{};   // Burner Generator only.
    float storedEnergy{};  // Battery Bank only.
};

struct PowerNetworkSummary {
    int networkCount{};
    float generation{};
    float demand{};
    float supplied{};
    float batteryStored{};
    float batteryCapacity{};
    int poweredLoads{};
    int shedLoads{};
};

class BaseInfrastructure {
public:
    explicit BaseInfrastructure(std::uint64_t worldSeed = 0);

    std::uint64_t place(MachineType type, IVec3 anchor);
    bool restore(const MachineObject& object);
    bool remove(std::uint64_t stableId);
    MachineObject* find(std::uint64_t stableId);
    const MachineObject* find(std::uint64_t stableId) const;
    MachineObject* nearest(MachineType type, Vec3 position, float maxDistance);

    void update(float dt);
    bool oxygenatedAt(const World& world, IVec3 cell, int maxRoomCells = 8192) const;
    PowerNetworkSummary summary() const { return summary_; }

    const std::vector<MachineObject>& objects() const { return objects_; }

private:
    std::uint64_t worldSeed_{};
    std::uint64_t nextSerial_{1};
    std::vector<MachineObject> objects_;
    PowerNetworkSummary summary_{};

    std::uint64_t allocateStableId();
};

} // namespace elysium
