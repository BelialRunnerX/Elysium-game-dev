#pragma once

#include "world/SurfaceInfrastructure.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace elysium {

// Stable non-block item IDs used by the first industrial processing graph.
// Raw ores continue to use their existing BlockType integer IDs, preserving
// compatibility with the player inventory and mining drops.
enum class IndustryItemId : int {
    CopperIngot = 3000,
    TinIngot = 3001,
    IronIngot = 3002,
    Carbon = 3003,
    BronzeIngot = 3004,
    SteelIngot = 3005,

    // Part Two v0.20 processing intermediates/components. IDs are appended and
    // stable so future recipe/data migration does not reinterpret old saves.
    StoneAggregate = 3006,
    CopperConcentrate = 3007,
    TinConcentrate = 3008,
    IronConcentrate = 3009,
    Sealant = 3010,
    CopperWire = 3011,
    SteelFrame = 3012,
    Actuator = 3013,
    ControlCircuit = 3014,
    SensorPackage = 3015,
    TurretAmmo = 3016,
    RepairKit = 3017,
    FilterCartridge = 3018,
    MachineCasing = 3019,
    CompositePanel = 3020
};

const char* industryItemName(int itemId);
bool validIndustryItemId(int itemId);

enum class SurfaceRecipeId : int {
    None = 0,
    SmeltCopper = 1,
    SmeltTin = 2,
    SmeltIron = 3,
    CharCoal = 4,
    AlloyBronze = 5,
    AlloySteel = 6,
    RefineCopper = 7,
    RefineTin = 8,
    RefineIron = 9,

    // Preprocessing and fabrication expansion.
    CrushStone = 10,
    CrushCopper = 11,
    CrushTin = 12,
    CrushIron = 13,
    RefineCopperConcentrate = 14,
    RefineTinConcentrate = 15,
    RefineIronConcentrate = 16,
    MixSealant = 17,
    FabricateCopperWire = 18,
    FabricateSteelFrame = 19,
    FabricateActuator = 20,
    FabricateControlCircuit = 21,
    FabricateSensorPackage = 22,
    FabricateTurretAmmo = 23,
    FabricateRepairKit = 24,
    FabricateFilterCartridge = 25,
    FabricateMachineCasing = 26,
    FabricateCompositePanel = 27
};

struct SurfaceRecipeIngredient {
    int itemId{};
    int count{};
};

struct SurfaceRecipeDefinition {
    SurfaceRecipeId id{SurfaceRecipeId::None};
    MachineType machine{MachineType::Furnace};
    std::string_view name;
    std::array<SurfaceRecipeIngredient,3> inputs{};
    int inputCount{};
    int outputItemId{};
    int outputCount{};
    float processSeconds{};
};

const SurfaceRecipeDefinition* surfaceRecipe(SurfaceRecipeId id);
std::vector<const SurfaceRecipeDefinition*> surfaceRecipesForMachine(MachineType machine);

struct SurfaceIndustryTelemetry {
    int processMachines{};
    int activeProcesses{};
    int stalledForInput{};
    int stalledForOutput{};
    int stalledForPower{};
    int completedProcesses{};
    int itemsConsumed{};
    int itemsProduced{};
    int networkPulls{};
    int networkPushes{};
    int furnaceFuelLoads{};

    // Part Two v0.20 explicit bulk logistics. Network Storage remains the
    // simple shared-base abstraction; these counters describe optional
    // physical throughput machines layered on top of it.
    int logisticsMachines{};
    int logisticsTransfers{};
    int conveyorTransfers{};
    int sorterTransfers{};
    int cargoTransfers{};
    int logisticsItemsMoved{};
    int logisticsJams{};
    int logisticsInvalidLinks{};
    int logisticsStalledForPower{};

    // Automated extraction is a bounded world-edit producer. Suspicion is
    // emitted as telemetry so the campaign/Empire layer remains the authority
    // over territorial standing rather than being hard-wired into industry.
    int extractorMachines{};
    int extractorCycles{};
    int extractorOreMined{};
    int extractorOutputStalls{};
    int extractorNoOre{};
    float extractorSuspicionGenerated{};

    // Defense logistics bridge: Fabricator ammunition becomes an actual base
    // resupply commodity rather than a dead-end recipe output.
    int turretAmmoLoads{};
    int turretRoundsLoaded{};
};

// Data-oriented industry service. Persistent state lives on SurfaceMachineObject;
// this system only executes deterministic processing and shared-inventory rules.
// Network Storage is the intentionally simple logistics abstraction from the
// design specification: powered machines on the same scalar power network may
// pull from and push to powered Network Storage without requiring a conveyor
// simulator for ordinary crafting. Conveyor/Sorter/CargoLoader form the
// optional explicit bulk-throughput layer. Crusher/ChemicalVat/Fabricator add
// preprocessing/fabrication, while Extractor is the bounded world-edit producer.
class SurfaceIndustrySystem {
public:
    static constexpr int MaxProcessStacks = 8;
    static constexpr int MaxStorageCrateStacks = 24;
    static constexpr int MaxNetworkStorageStacks = 64;
    static constexpr int MaxStackCount = 999;
    static constexpr float FurnaceCoalSeconds = 30.0f;
    static constexpr float ConveyorTransferSeconds = 0.50f;
    static constexpr float SorterTransferSeconds = 0.75f;
    static constexpr float CargoLoaderTransferSeconds = 1.00f;
    static constexpr int CargoLoaderBatch = 8;
    static constexpr int MaxLogisticsTransfersPerUpdate = 8;
    static constexpr float MaxLogisticsLinkDistance = 2.25f;
    static constexpr float ExtractorCycleSeconds = 2.0f;
    static constexpr int ExtractorHorizontalRadius = 3;
    static constexpr int ExtractorDepth = 12;
    static constexpr int MaxExtractorCyclesPerUpdate = 4;
    static constexpr float ExtractorSuspicionPerOre = 1.0f; // tuning: industrial activity pressure

    bool insert(SurfaceInfrastructure& infrastructure,
                std::uint64_t machineStableId,
                int itemId,
                int count);
    int extract(SurfaceInfrastructure& infrastructure,
                std::uint64_t machineStableId,
                int itemId,
                int count);
    int itemCount(const SurfaceInfrastructure& infrastructure,
                  std::uint64_t machineStableId,
                  int itemId) const;

    bool selectRecipe(SurfaceInfrastructure& infrastructure,
                      std::uint64_t machineStableId,
                      SurfaceRecipeId recipe);

    // Configure one explicit bulk-logistics machine. Source/target identities
    // are stable infrastructure IDs; spatial validation uses curved planet
    // positions and therefore works across cube-face seams without edge tables.
    bool configureLogisticsLink(SurfaceInfrastructure& infrastructure,
                                const PlanetSurface& planet,
                                std::uint64_t transportStableId,
                                std::uint64_t sourceStableId,
                                std::uint64_t targetStableId,
                                std::uint64_t alternateTargetStableId = 0,
                                int sorterFilterItemId = 0);
    bool clearLogisticsLink(SurfaceInfrastructure& infrastructure,
                            std::uint64_t transportStableId);

    void update(SurfaceInfrastructure& infrastructure, float dt);

    // World-mutating automated mining is kept explicit because the ordinary
    // processing pass above is pure infrastructure/inventory behavior. The
    // Extractor only scans a small address-bounded neighborhood, crosses seams
    // through PlanetSurface::normalize/project ownership, ignores player-placed
    // ore, and persists the actual excavation as normal voxel edits.
    void updateExtraction(PlanetSurface& planet,
                          SurfaceInfrastructure& infrastructure,
                          float dt);

    const SurfaceIndustryTelemetry& telemetry() const { return telemetry_; }

    // Helpers used by persistence validation and tests. Inventory is kept
    // normalized: strictly positive stacks sorted by stable item ID.
    static bool normalizeInventory(SurfaceMachineObject& machine);
    static int inventoryCapacity(const SurfaceMachineObject& machine);

private:
    SurfaceIndustryTelemetry telemetry_{};

    static bool processMachine(MachineType type);
    static bool poweredProcessMachine(MachineType type);
    static bool furnaceMachine(MachineType type);
    static bool logisticsMachine(MachineType type);
    static float logisticsInterval(MachineType type);
    static int logisticsBatch(MachineType type);
    static int localCount(const SurfaceMachineObject& machine, int itemId);
    static int localInsert(SurfaceMachineObject& machine, int itemId, int count);
    static int localExtract(SurfaceMachineObject& machine, int itemId, int count);
    static bool localCanInsert(const SurfaceMachineObject& machine, int itemId, int count);
    static bool logisticsCanExtractItem(const SurfaceMachineObject& machine, int itemId);
    static bool logisticsCanReceiveItem(const SurfaceMachineObject& machine, int itemId);

    static SurfaceMachineObject* firstPoweredNetworkStorage(SurfaceInfrastructure& infrastructure,
                                                             std::uint64_t networkId,
                                                             std::uint64_t excludeStableId = 0);
    static std::vector<SurfaceMachineObject*> poweredNetworkStorages(SurfaceInfrastructure& infrastructure,
                                                                     std::uint64_t networkId,
                                                                     std::uint64_t excludeStableId = 0);

    int availableInput(SurfaceInfrastructure& infrastructure,
                       SurfaceMachineObject& machine,
                       int itemId) const;
    bool consumeInput(SurfaceInfrastructure& infrastructure,
                      SurfaceMachineObject& machine,
                      int itemId,
                      int count);
    bool canRouteOutput(SurfaceInfrastructure& infrastructure,
                        SurfaceMachineObject& machine,
                        int itemId,
                        int count) const;
    bool routeOutput(SurfaceInfrastructure& infrastructure,
                     SurfaceMachineObject& machine,
                     int itemId,
                     int count);
    bool ensureFurnaceFuel(SurfaceInfrastructure& infrastructure,
                           SurfaceMachineObject& machine);
    const SurfaceRecipeDefinition* chooseRecipe(SurfaceInfrastructure& infrastructure,
                                                SurfaceMachineObject& machine) const;
    void updateLogistics(SurfaceInfrastructure& infrastructure, float dt);
    bool attemptLogisticsTransfer(SurfaceInfrastructure& infrastructure,
                                  SurfaceMachineObject& transport);
    void updateDefenseSupply(SurfaceInfrastructure& infrastructure);
    bool findExtractorTarget(const PlanetSurface& planet,
                             const SurfaceMachineObject& extractor,
                             SurfaceCellAddress& out) const;
};

} // namespace elysium
