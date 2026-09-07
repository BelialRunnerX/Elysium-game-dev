#include "world/ShipTransit.hpp"

#include <algorithm>
#include <cmath>

namespace elysium {
namespace {

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

} // namespace

ShipTransit::ShipTransit(int dockedPlanetIndex, PlanetClass dockedClass) {
    if (!validPlanetIndex(dockedPlanetIndex)) {
        dockedPlanetIndex = 0;
        dockedClass = PlanetClass::Temperate;
    }
    currentPlanet_ = dockedPlanetIndex;
    destinationPlanet_ = dockedPlanetIndex;
    currentClass_ = dockedClass;
    destinationClass_ = dockedClass;
}

std::string_view ShipTransit::phaseName(ShipFlightPhase phase) {
    switch (phase) {
        case ShipFlightPhase::Docked: return "Docked";
        case ShipFlightPhase::TakingOff: return "TakingOff";
        case ShipFlightPhase::Atmospheric: return "Atmospheric";
        case ShipFlightPhase::Orbital: return "Orbital";
        case ShipFlightPhase::InTransit: return "InTransit";
        case ShipFlightPhase::Descending: return "Descending";
    }
    return "Unknown";
}

std::string_view ShipTransit::rejectName(ShipTravelReject reject) {
    switch (reject) {
        case ShipTravelReject::None: return "None";
        case ShipTravelReject::NotDocked: return "NotDocked";
        case ShipTravelReject::AlreadyAirborne: return "AlreadyAirborne";
        case ShipTravelReject::SamePlanet: return "SamePlanet";
        case ShipTravelReject::InvalidPlanet: return "InvalidPlanet";
        case ShipTravelReject::HullBelowTakeoff: return "HullBelowTakeoff";
        case ShipTravelReject::CargoOverloaded: return "CargoOverloaded";
        case ShipTravelReject::NotOrbital: return "NotOrbital";
        case ShipTravelReject::Busy: return "Busy";
    }
    return "Unknown";
}

bool ShipTransit::validPlanetIndex(int index) {
    return index >= 0 && index < kPlanetCount;
}

PlanetClass ShipTransit::classForVerticalSliceIndex(int index) {
    switch (index) {
        case 0: return PlanetClass::Temperate;
        case 1: return PlanetClass::Barren;
        case 2: return PlanetClass::Scorched;
        default: return PlanetClass::Temperate;
    }
}

ShipTakeoffChecklist ShipTransit::checklist() const {
    ShipTakeoffChecklist c;
    c.hullIntegrity = hull_;
    c.minHullForTakeoff = kMinHullTakeoff;
    c.hullOk = hull_ + 1e-5f >= kMinHullTakeoff;
    c.cargoMassUnits = cargoMass_;
    c.maxCargoForTakeoff = maxCargo_;
    c.cargoOk = cargoMass_ <= maxCargo_ + 1e-5f;
    c.ready = c.hullOk && c.cargoOk;
    return c;
}

void ShipTransit::setHullIntegrity(float hull) {
    hull_ = clampf(hull, 0.0f, kHullMax);
}

void ShipTransit::applyHullDamage(float amount) {
    if (amount <= 0.0f) return;
    hull_ = clampf(hull_ - amount, 0.0f, kHullMax);
}

void ShipTransit::setCargoMassUnits(float mass) {
    cargoMass_ = std::max(0.0f, mass);
}

void ShipTransit::setMaxCargoForTakeoff(float maxMass) {
    maxCargo_ = std::max(0.0f, maxMass);
}

int ShipTransit::applyRepairKits(int kitCount) {
    if (kitCount <= 0 || hull_ >= kHullMax - 1e-5f) return 0;
    int used = 0;
    while (used < kitCount && hull_ < kHullMax - 1e-5f) {
        hull_ = clampf(hull_ + kRepairPerKit, 0.0f, kHullMax);
        ++used;
    }
    return used;
}

void ShipTransit::setReject(ShipTravelReject reason) {
    lastReject_ = reason;
}

void ShipTransit::enterPhase(ShipFlightPhase next) {
    phase_ = next;
    phaseElapsed_ = 0.0f;
}

float ShipTransit::phaseDuration(ShipFlightPhase phase) const {
    switch (phase) {
        case ShipFlightPhase::TakingOff: return kTakingOffDuration;
        case ShipFlightPhase::Atmospheric: return kAtmosphericDuration;
        case ShipFlightPhase::InTransit: return kTransitDuration;
        case ShipFlightPhase::Descending: return kDescendingDuration;
        case ShipFlightPhase::Docked:
        case ShipFlightPhase::Orbital:
            return 0.0f;
    }
    return 0.0f;
}

bool ShipTransit::beginTakeoff() {
    if (phase_ != ShipFlightPhase::Docked) {
        setReject(ShipTravelReject::AlreadyAirborne);
        return false;
    }
    const auto check = checklist();
    if (!check.hullOk) {
        setReject(ShipTravelReject::HullBelowTakeoff);
        return false;
    }
    if (!check.cargoOk) {
        setReject(ShipTravelReject::CargoOverloaded);
        return false;
    }
    setReject(ShipTravelReject::None);
    destinationPlanet_ = currentPlanet_;
    destinationClass_ = currentClass_;
    enterPhase(ShipFlightPhase::TakingOff);
    return true;
}

bool ShipTransit::beginTransit(int destPlanetIndex, PlanetClass destClass) {
    if (phase_ != ShipFlightPhase::Orbital) {
        setReject(ShipTravelReject::NotOrbital);
        return false;
    }
    if (!validPlanetIndex(destPlanetIndex)) {
        setReject(ShipTravelReject::InvalidPlanet);
        return false;
    }
    if (destPlanetIndex == currentPlanet_) {
        setReject(ShipTravelReject::SamePlanet);
        return false;
    }
    setReject(ShipTravelReject::None);
    destinationPlanet_ = destPlanetIndex;
    destinationClass_ = destClass;
    enterPhase(ShipFlightPhase::InTransit);
    return true;
}

void ShipTransit::tick(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (phase_ == ShipFlightPhase::Docked || phase_ == ShipFlightPhase::Orbital) {
        return;
    }

    phaseElapsed_ += dt;
    const float need = phaseDuration(phase_);
    if (phaseElapsed_ + 1e-6f < need) return;

    switch (phase_) {
        case ShipFlightPhase::TakingOff:
            enterPhase(ShipFlightPhase::Atmospheric);
            break;
        case ShipFlightPhase::Atmospheric:
            enterPhase(ShipFlightPhase::Orbital);
            break;
        case ShipFlightPhase::InTransit:
            enterPhase(ShipFlightPhase::Descending);
            break;
        case ShipFlightPhase::Descending:
            currentPlanet_ = destinationPlanet_;
            currentClass_ = destinationClass_;
            enterPhase(ShipFlightPhase::Docked);
            break;
        case ShipFlightPhase::Docked:
        case ShipFlightPhase::Orbital:
            break;
    }
}

bool ShipTransit::advanceUntilDocked(float stepDt, int maxSteps) {
    if (stepDt <= 0.0f) stepDt = 0.5f;
    if (maxSteps < 1) maxSteps = 1;
    for (int i = 0; i < maxSteps; ++i) {
        if (phase_ == ShipFlightPhase::Docked) return true;
        // Orbital is a hold — travelTo/beginTransit must choose a destination.
        if (phase_ == ShipFlightPhase::Orbital) return false;
        tick(stepDt);
    }
    return phase_ == ShipFlightPhase::Docked;
}

bool ShipTransit::travelTo(int destPlanetIndex, PlanetClass destClass,
                           float stepDt, int maxSteps) {
    if (!validPlanetIndex(destPlanetIndex)) {
        setReject(ShipTravelReject::InvalidPlanet);
        return false;
    }
    if (phase_ != ShipFlightPhase::Docked) {
        setReject(airborne() ? ShipTravelReject::Busy : ShipTravelReject::NotDocked);
        return false;
    }
    if (destPlanetIndex == currentPlanet_) {
        setReject(ShipTravelReject::SamePlanet);
        return false;
    }
    if (!beginTakeoff()) return false;
    if (!advanceUntilDocked(stepDt, maxSteps)) {
        // Should be Orbital after takeoff climb.
        if (phase_ != ShipFlightPhase::Orbital) {
            setReject(ShipTravelReject::Busy);
            return false;
        }
    }
    if (phase_ == ShipFlightPhase::Docked) {
        // Degenerate: advanceUntilDocked returned early somehow.
        setReject(ShipTravelReject::Busy);
        return false;
    }
    if (!beginTransit(destPlanetIndex, destClass)) return false;
    if (!advanceUntilDocked(stepDt, maxSteps)) {
        setReject(ShipTravelReject::Busy);
        return false;
    }
    setReject(ShipTravelReject::None);
    return phase_ == ShipFlightPhase::Docked && currentPlanet_ == destPlanetIndex;
}

} // namespace elysium
