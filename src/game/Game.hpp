#pragma once

#include "core/JobSystem.hpp"
#include "ecs/EcsWorld.hpp"
#include "render/RaylibGraphicsBackend.hpp"
#include "render/WorldRenderer.hpp"
#include "render/PlanetSurfaceRenderer.hpp"
#include "world/BaseInfrastructure.hpp"
#include "world/PlanetSurface.hpp"
#include "world/SurfaceInfrastructure.hpp"
#include "world/SurfaceIndustry.hpp"
#include "world/SurfaceNavigation.hpp"
#include "world/SurfaceSiege.hpp"
#include "world/SurfaceSuspicion.hpp"
#include "world/ShipTransit.hpp"
#include "world/RegistryBeaconClaim.hpp"
#include "world/SurfaceWorldRead.hpp"
#include "world/World.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <raylib.h>

namespace elysium {

class Game {
public:
    Game();
    ~Game();

    void run();

private:
    static constexpr int PlanetCount = 3;

    std::uint64_t galaxySeed_{0xE1A51A5EULL};
    std::array<std::unique_ptr<World>, PlanetCount> worlds_;
    std::array<std::unique_ptr<PlanetSurface>, PlanetCount> planetSurfaces_;
    std::array<std::unique_ptr<BaseInfrastructure>, PlanetCount> bases_;
    std::array<std::unique_ptr<SurfaceInfrastructure>, PlanetCount> surfaceBases_;
    std::array<std::unique_ptr<SurfaceNavigationService>, PlanetCount> surfaceNavigation_;
    std::array<std::unique_ptr<SurfaceSiegeDirector>, PlanetCount> surfaceSieges_;
    int currentPlanet_{};
    EcsWorld ecs_;
    JobSystem jobs_;
    RaylibGraphicsBackend graphics_;
    WorldRenderer renderer_;
    PlanetSurfaceRenderer planetRenderer_;
    Camera3D camera_{};

    float yaw_{};
    float pitch_{};
    bool orbitalPreview_{};
    bool sphericalSurfaceMode_{true};
    float orbitYaw_{0.7f};
    float orbitPitch_{0.35f};
    float orbitDistance_{92.0f};
    Vec3 orbitWalkerPosition_{};
    std::array<Vec3, PlanetCount> sphericalPlayerPositions_{};
    float sphericalVerticalVelocity_{};
    bool sphericalGrounded_{true};
    SurfaceCellAddress sphericalMiningCell_{};
    bool sphericalMiningValid_{};
    SuspicionLedger suspicionLedger_{};
    ShipTransit shipTransit_{};
    float favor_{};
    std::uint64_t saveGeneration_{};
    SurfaceAiTelemetry surfaceAiTelemetry_{};
    SurfaceDefenseTelemetry surfaceDefenseTelemetry_{};
    SurfaceIndustrySystem surfaceIndustry_{};
    SurfaceIndustryTelemetry surfaceIndustryTelemetry_{};
    RegistryBeaconAuthority registryBeacons_{galaxySeed_};
    int toolTier_{0};
    BlockType selectedBlock_{BlockType::Dirt};
    bool travelMenu_{};
    bool craftingMenu_{};
    bool mapMenu_{};
    bool inventoryMenu_{};
    int inventoryTab_{};
    int inventorySelected_{};
    bool sculptMode_{};
    bool machineMode_{};
    bool portalMode_{};
    SurfacePortalType selectedPortal_{SurfacePortalType::Door};
    MachineType selectedMachine_{MachineType::BurnerGenerator};
    float miningProgress_{};
    IVec3 miningCell_{-999,-999,-999};
    float dispatchTimer_{};
    std::uint64_t dispatchRollCounter_{};
    float messageTimer_{};
    std::string message_;

    World& world() { return *worlds_[static_cast<std::size_t>(currentPlanet_)]; }
    const World& world() const { return *worlds_[static_cast<std::size_t>(currentPlanet_)]; }
    BaseInfrastructure& base() { return *bases_[static_cast<std::size_t>(currentPlanet_)]; }
    const BaseInfrastructure& base() const { return *bases_[static_cast<std::size_t>(currentPlanet_)]; }
    SurfaceInfrastructure& surfaceBase() { return *surfaceBases_[static_cast<std::size_t>(currentPlanet_)]; }
    const SurfaceInfrastructure& surfaceBase() const { return *surfaceBases_[static_cast<std::size_t>(currentPlanet_)]; }
    SurfaceNavigationService& surfaceNavigation() { return *surfaceNavigation_[static_cast<std::size_t>(currentPlanet_)]; }
    const SurfaceNavigationService& surfaceNavigation() const { return *surfaceNavigation_[static_cast<std::size_t>(currentPlanet_)]; }
    SurfaceSiegeDirector& surfaceSiege() { return *surfaceSieges_[static_cast<std::size_t>(currentPlanet_)]; }
    const SurfaceSiegeDirector& surfaceSiege() const { return *surfaceSieges_[static_cast<std::size_t>(currentPlanet_)]; }
    PlanetSurface& planetSurface() { return *planetSurfaces_[static_cast<std::size_t>(currentPlanet_)]; }
    const PlanetSurface& planetSurface() const { return *planetSurfaces_[static_cast<std::size_t>(currentPlanet_)]; }
    SurfaceWorldReadService surfaceRead() const { return SurfaceWorldReadService(planetSurface(),planetRenderer_.cpuChunkCache()); }

    void initWorlds();
    Vec3 spawnPoint(const World& w) const;
    Vec3 shipTerminalPosition(const World& w) const;
    Vec3 sphericalShipTerminalPosition() const;
    Vec3 sphericalArrivalPosition() const;
    Vec3 cameraPosition() const;
    Vec3 cameraForward() const;
    Vec3 sphericalCameraPosition() const;
    Vec3 sphericalCameraForward() const;

    void update(float dt);
    void updateMenus();
    void updatePlayer(float dt);
    void updateOrbitalPreview(float dt);
    void updateSphericalPlayer(float dt);
    void updateSphericalInteraction(float dt);
    void updateInteraction(float dt);
    void updateEmpire(float dt);
    void switchPlanet(int index);

    void draw();
    void drawWorld3D();
    void drawOrbitalPreview();
    void drawSphericalSurface();
    void drawSphericalHud();
    void drawSurfaceMachines3D();
    void drawSurfacePortals3D();
    void drawSurfaceShipTerminal3D();
    void drawHud();
    void drawOrbitalHud();
    void drawTravelMenu();
    void drawCraftingMenu();
    void drawMapMenu();
    void drawInventoryMenu();
    void drawShipTerminal3D();
    void drawMachines3D();

    void setMessage(std::string text, float seconds = 2.5f);
    void selectHotbar();
    void tryPlaceBlock();
    void trySphericalPlaceBlock();
    void trySphericalMine(float dt);
    void trySphericalMicroSculpt();
    void trySphericalAttack();
    void trySphericalPlaceMachine();
    void trySphericalPlacePortal();
    bool trySurfaceMachineInteract();
    bool trySurfacePortalInteract();
    void tryPlaceMachine();
    bool tryMachineInteract();
    void tryMine(float dt);
    void tryMicroSculpt();
    void tryAttack();
    void tryCraft(int recipe);
    bool nearShip() const;

    float claimFloor() const;
    int placedStructureCount(const World& w) const;
    SuspicionState& suspicion();
    void raiseSuspicion(float amount);

    void saveGame();
    bool loadGame();
    void resetPlayerMetersAtShip();
    bool tryShipRepairFromInventory();
    void syncShipCargoFromInventory();
};

} // namespace elysium
