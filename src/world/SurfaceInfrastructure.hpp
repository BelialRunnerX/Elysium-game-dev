#pragma once

#include "world/BaseInfrastructure.hpp"
#include "world/PlanetSurface.hpp"

#include <cstdint>
#include <vector>

namespace elysium {

class SurfaceIndustrySystem;

enum class SurfacePortalType : std::uint8_t { Door = 0, Airlock = 1 };

enum class SurfaceAirlockState : std::uint8_t {
    Idle = 0,
    Depressurizing = 1,
    ExteriorOpen = 2,
    Pressurizing = 3,
    InteriorOpen = 4,
    Fault = 5
};

enum class SurfaceAirlockDestination : std::uint8_t { Interior = 0, Exterior = 1 };

enum class SurfaceAutomationTrigger : std::uint8_t {
    HostilesDetected = 0,
    BatteryBelow = 1,
    AtmospherePressureBelow = 2
};

enum class SurfaceAutomationAction : std::uint8_t {
    EnableMachine = 0,
    DisableMachine = 1,
    ClosePortal = 2
};

constexpr int surfacePortalItemId(SurfacePortalType type) {
    return 2000 + static_cast<int>(type);
}

const char* surfacePortalName(SurfacePortalType type);
BlockType surfacePortalBlockType(SurfacePortalType type);
const char* surfaceAirlockStateName(SurfaceAirlockState state);
const char* surfaceAutomationTriggerName(SurfaceAutomationTrigger trigger);
const char* surfaceAutomationActionName(SurfaceAutomationAction action);

struct SurfacePortalObject {
    std::uint64_t stableId{};
    SurfacePortalType type{SurfacePortalType::Door};
    SurfaceCellAddress anchor{};
    bool open{};
};

struct SurfaceItemStack {
    int itemId{};
    int count{};

    friend bool operator==(const SurfaceItemStack&, const SurfaceItemStack&) = default;
};

struct SurfaceMachineObject {
    std::uint64_t stableId{};
    MachineType type{MachineType::StorageCrate};
    SurfaceCellAddress anchor{};
    bool enabled{true};
    bool powered{};
    float fuelSeconds{};
    float storedEnergy{};
    // Bounded local atmosphere state owned by an Atmosphere Unit. These
    // scalars are persistent; roomSealed is derived from current geometry.
    float roomPressure{};
    float roomOxygen{};
    bool roomSealed{};

    // Part Two v0.15 defense state. Ammo and shield charge are persistent.
    // Cooldown, network membership, and sensor counts are derived runtime data.
    int ammo{};
    float shieldCharge{};
    float cooldownSeconds{};
    std::uint64_t powerNetworkId{};
    int detectedHostiles{};

    // Part Two v0.19 industrial state. Inventories are intentionally stored on
    // stable machine objects so processing survives save/load and can be
    // translated into the coworker build's sparse infrastructure journal.
    // Runtime systems keep the vector normalized (positive counts, sorted IDs).
    std::vector<SurfaceItemStack> inventory;
    int selectedRecipeId{};
    int activeRecipeId{};
    float processProgressSeconds{};
    float extractorProgressSeconds{};
    int sorterFilterItemId{};

    // Part Two v0.20 explicit bulk-logistics routing. These are stable object
    // references, never dense planet indices. Conveyor/CargoLoader use source
    // + target; Sorter additionally uses alternateTarget and sorterFilterItemId.
    // Progress persists so save/load cannot duplicate throughput. blocked is
    // derived runtime telemetry and is deliberately not authoritative.
    std::uint64_t logisticsSourceStableId{};
    std::uint64_t logisticsTargetStableId{};
    std::uint64_t logisticsAlternateTargetStableId{};
    float logisticsProgressSeconds{};
    bool logisticsBlocked{};
};

struct SurfaceHostileContact {
    std::uint64_t stableId{};
    Vec3 position{};
    float health{};
};

struct SurfaceTurretFireRequest {
    std::uint64_t turretStableId{};
    std::uint64_t targetStableId{};
    float damage{};
};

struct SurfaceDefenseTelemetry {
    int poweredSensors{};
    int hostilesDetected{};
    int activeTurrets{};
    int fireRequests{};
    int poweredShields{};
    float shieldCharge{};
    float shieldCapacity{};
    int automationRulesEvaluated{};
    int automationActionsApplied{};
};

struct SurfaceAutomationRule {
    std::uint64_t stableId{};
    std::uint64_t controllerMachineId{};
    SurfaceAutomationTrigger trigger{SurfaceAutomationTrigger::HostilesDetected};
    std::uint64_t sourceStableId{};
    float threshold{};
    SurfaceAutomationAction action{SurfaceAutomationAction::EnableMachine};
    std::uint64_t targetStableId{};
    bool enabled{true};
};

// Stable controller record for a two-door powered airlock chamber. The portals
// remain authoritative voxel objects; this record only owns the interlock and
// bounded chamber atmosphere state.
struct SurfaceAirlockAssembly {
    std::uint64_t stableId{};
    std::uint64_t controllerMachineId{};
    std::uint64_t innerPortalId{};
    std::uint64_t outerPortalId{};
    SurfaceCellAddress chamberAnchor{};
    SurfaceAirlockState state{SurfaceAirlockState::Idle};
    float chamberPressure{1.0f};
    float chamberOxygen{1.0f};
};

struct SurfaceAtmosphereSample {
    float pressure{};
    float oxygen{};
    bool sealed{};
    bool poweredSource{};
    bool airlockChamber{};

    bool breathable() const { return pressure >= 0.55f && oxygen >= 0.45f; }
};

// Cube-sphere counterpart to BaseInfrastructure. It preserves the same scalar
// power-network rules while anchors use stable planetary addresses rather than
// planar coordinates. Persistent state uses stable IDs; runtime behavior remains
// system-owned and can consume ECS-derived contacts without storing ECS IDs.
class SurfaceInfrastructure {
public:
    explicit SurfaceInfrastructure(std::uint64_t worldSeed = 0);

    std::uint64_t place(MachineType type, SurfaceCellAddress anchor);
    bool restore(const SurfaceMachineObject& object);
    bool remove(std::uint64_t stableId);
    SurfaceMachineObject* find(std::uint64_t stableId);
    const SurfaceMachineObject* find(std::uint64_t stableId) const;
    SurfaceMachineObject* nearest(const PlanetSurface& planet, MachineType type, Vec3 position, float maxDistance);
    bool occupied(SurfaceCellAddress anchor) const;

    std::uint64_t placePortal(PlanetSurface& planet, SurfacePortalType type, SurfaceCellAddress anchor, bool open = false);
    bool restorePortal(const SurfacePortalObject& object);
    bool removePortal(PlanetSurface& planet, std::uint64_t stableId, bool clearCell = true);
    bool setPortalOpen(PlanetSurface& planet, std::uint64_t stableId, bool open);
    SurfacePortalObject* findPortal(std::uint64_t stableId);
    const SurfacePortalObject* findPortal(std::uint64_t stableId) const;
    SurfacePortalObject* nearestPortal(const PlanetSurface& planet, Vec3 position, float maxDistance);
    bool portalOccupied(SurfaceCellAddress anchor) const;

    std::uint64_t createAirlockAssembly(PlanetSurface& planet,
                                        std::uint64_t controllerMachineId,
                                        std::uint64_t innerPortalId,
                                        std::uint64_t outerPortalId,
                                        SurfaceCellAddress chamberAnchor,
                                        float initialPressure = 1.0f,
                                        float initialOxygen = 1.0f);
    bool restoreAirlockAssembly(const SurfaceAirlockAssembly& assembly);
    bool removeAirlockAssembly(std::uint64_t stableId);
    SurfaceAirlockAssembly* findAirlockAssembly(std::uint64_t stableId);
    const SurfaceAirlockAssembly* findAirlockAssembly(std::uint64_t stableId) const;
    SurfaceAirlockAssembly* airlockForController(std::uint64_t controllerMachineId);
    const SurfaceAirlockAssembly* airlockForController(std::uint64_t controllerMachineId) const;
    SurfaceAirlockAssembly* airlockForPortal(std::uint64_t portalId);
    bool requestAirlockCycle(PlanetSurface& planet, std::uint64_t stableId, SurfaceAirlockDestination destination);

    std::uint64_t createAutomationRule(std::uint64_t controllerMachineId,
                                       SurfaceAutomationTrigger trigger,
                                       std::uint64_t sourceStableId,
                                       float threshold,
                                       SurfaceAutomationAction action,
                                       std::uint64_t targetStableId);
    bool restoreAutomationRule(const SurfaceAutomationRule& rule);
    bool removeAutomationRule(std::uint64_t stableId);
    SurfaceAutomationRule* findAutomationRule(std::uint64_t stableId);
    const SurfaceAutomationRule* findAutomationRule(std::uint64_t stableId) const;

    void update(PlanetSurface& planet, float dt);
    SurfaceDefenseTelemetry updateDefense(PlanetSurface& planet, float dt, const std::vector<SurfaceHostileContact>& contacts);
    std::vector<SurfaceTurretFireRequest> consumeTurretFireRequests();
    float absorbShieldDamage(const PlanetSurface& planet, Vec3 protectedPosition, float incomingDamage);

    SurfaceAtmosphereSample atmosphereAt(const PlanetSurface& planet, Vec3 position, int maxRoomCells = 8192) const;
    bool oxygenatedAt(const PlanetSurface& planet, Vec3 position, int maxRoomCells = 8192) const;
    PowerNetworkSummary summary() const { return summary_; }
    SurfaceDefenseTelemetry defenseTelemetry() const { return defenseTelemetry_; }
    const std::vector<SurfaceMachineObject>& objects() const { return objects_; }
    const std::vector<SurfacePortalObject>& portals() const { return portals_; }
    const std::vector<SurfaceAirlockAssembly>& airlockAssemblies() const { return airlocks_; }
    const std::vector<SurfaceAutomationRule>& automationRules() const { return automationRules_; }

private:
    friend class SurfaceIndustrySystem;

    std::uint64_t worldSeed_{};
    std::uint64_t nextSerial_{1};
    std::vector<SurfaceMachineObject> objects_;
    std::vector<SurfacePortalObject> portals_;
    std::vector<SurfaceAirlockAssembly> airlocks_;
    std::vector<SurfaceAutomationRule> automationRules_;
    std::vector<SurfaceTurretFireRequest> turretFireRequests_;
    PowerNetworkSummary summary_{};
    SurfaceDefenseTelemetry defenseTelemetry_{};

    std::uint64_t allocateStableId();
    bool setPortalOpenUnchecked(PlanetSurface& planet, std::uint64_t stableId, bool open);
    void updateAirlocks(PlanetSurface& planet, float dt);
    void evaluateAutomation(PlanetSurface& planet);
};

} // namespace elysium
