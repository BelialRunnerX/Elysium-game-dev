#pragma once

#include "world/PlanetTypes.hpp"

#include <cstdint>
#include <string_view>

namespace elysium {

// Part 24 Ship pillar — portable travel authority (no EnTT, no raylib, no planar
// World). Game wires inventory/UI; headless tests own the phase graph.

enum class ShipFlightPhase : std::uint8_t {
    Docked = 0,
    TakingOff,
    Atmospheric,
    Orbital,
    InTransit,
    Descending
};

enum class ShipTravelReject : std::uint8_t {
    None = 0,
    NotDocked,
    AlreadyAirborne,
    SamePlanet,
    InvalidPlanet,
    HullBelowTakeoff,
    CargoOverloaded,
    NotOrbital,
    Busy
};

// Visible takeoff envelope (Gate 4). Extra fields reserved for gravity/thruster
// content without inventing a second checklist API.
struct ShipTakeoffChecklist {
    float hullIntegrity{};
    float minHullForTakeoff{};
    bool hullOk{};
    float cargoMassUnits{};
    float maxCargoForTakeoff{};
    bool cargoOk{};
    bool ready{};
};

class ShipTransit {
public:
    static constexpr int kPlanetCount = 3;
    static constexpr float kHullMax = 100.0f;
    static constexpr float kMinHullTakeoff = 40.0f;
    static constexpr float kRepairPerKit = 25.0f;
    static constexpr float kDefaultMaxCargo = 50.0f;

    // Deterministic phase durations (seconds). Tests and Game may tick with
    // large dt; the graph order is fixed.
    static constexpr float kTakingOffDuration = 1.0f;
    static constexpr float kAtmosphericDuration = 1.0f;
    static constexpr float kTransitDuration = 2.0f;
    static constexpr float kDescendingDuration = 1.0f;

    explicit ShipTransit(int dockedPlanetIndex = 0,
                         PlanetClass dockedClass = PlanetClass::Temperate);

    ShipFlightPhase phase() const { return phase_; }
    int currentPlanetIndex() const { return currentPlanet_; }
    int destinationPlanetIndex() const { return destinationPlanet_; }
    PlanetClass currentPlanetClass() const { return currentClass_; }
    PlanetClass destinationPlanetClass() const { return destinationClass_; }
    float hullIntegrity() const { return hull_; }
    float cargoMassUnits() const { return cargoMass_; }
    float maxCargoForTakeoff() const { return maxCargo_; }
    float phaseElapsed() const { return phaseElapsed_; }
    ShipTravelReject lastReject() const { return lastReject_; }
    bool airborne() const { return phase_ != ShipFlightPhase::Docked; }

    static std::string_view phaseName(ShipFlightPhase phase);
    static std::string_view rejectName(ShipTravelReject reject);
    static bool validPlanetIndex(int index);
    static PlanetClass classForVerticalSliceIndex(int index);

    ShipTakeoffChecklist checklist() const;

    void setHullIntegrity(float hull);
    void applyHullDamage(float amount);
    void setCargoMassUnits(float mass);
    void setMaxCargoForTakeoff(float maxMass);

    // Each kit restores kRepairPerKit hull (capped). Returns kits actually used.
    int applyRepairKits(int kitCount);

    // Fail-closed. Docked + checklist.ready → TakingOff.
    bool beginTakeoff();

    // Orbital only. Records destination; enters InTransit.
    bool beginTransit(int destPlanetIndex, PlanetClass destClass);

    // Advance the phase graph. Safe to call while Docked (no-op).
    void tick(float dt);

    // Walk the machine until Docked or step budget exhausted.
    bool advanceUntilDocked(float stepDt, int maxSteps);

    // Repair-gated full inter-planet trip that walks Docked→…→Docked(dest).
    // Uses the real phase graph (not a parallel teleport). No planar World.
    bool travelTo(int destPlanetIndex, PlanetClass destClass,
                  float stepDt = 0.5f, int maxSteps = 64);

private:
    void setReject(ShipTravelReject reason);
    void enterPhase(ShipFlightPhase next);
    float phaseDuration(ShipFlightPhase phase) const;

    ShipFlightPhase phase_{ShipFlightPhase::Docked};
    int currentPlanet_{0};
    int destinationPlanet_{0};
    PlanetClass currentClass_{PlanetClass::Temperate};
    PlanetClass destinationClass_{PlanetClass::Temperate};
    float hull_{kHullMax};
    float cargoMass_{0.0f};
    float maxCargo_{kDefaultMaxCargo};
    float phaseElapsed_{0.0f};
    ShipTravelReject lastReject_{ShipTravelReject::None};
};

} // namespace elysium
