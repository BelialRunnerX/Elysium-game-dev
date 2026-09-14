#pragma once

#include <cstdint>
#include <string_view>

namespace elysium {

// Part 24 Survival — portable pressure → decision policy (no EnTT, no raylib,
// no Presentation). Intentionally separate from PlanetEnvironment so the O2
// blend seam (effectiveOxygenDrain) stays atmosphere-only.
//
// Sense/Game: compute effectiveOxygenDrain + hazard, then call
// evaluateSurvivalPressure on current meters. ECS Commit still owns meter
// mutation; this module only answers "what must the player decide to do?".

struct SurvivalRateTable {
    float hungerDrainPerSecond{0.12f};
    float energySprintDrainPerSecond{13.0f};
    float energyRecoverPerSecond{8.0f};
    float asphyxiaDamagePerSecond{9.0f};
    float starvationDamagePerSecond{1.5f};
};

// Canonical metabolic / suit rates. Must stay numerically aligned with
// EcsWorld::applyVitals / updateVitals energy+hunger math (ecs does not
// include world/; values are duplicated there with a cross-reference).
const SurvivalRateTable& survivalRateTable();

struct SurvivalMeters {
    float health{100.0f};
    float oxygen{100.0f};
    float energy{100.0f};
    float hunger{100.0f};
};

struct SurvivalContext {
    // Caller applies effectiveOxygenDrain(...) first — never re-blend O2 here.
    float effectiveOxygenDrainPerSecond{};
    float hazardDamagePerSecond{};
    bool sprinting{};
    bool moving{};
};

enum class SurvivalUrgency : std::uint8_t {
    None = 0,
    Advise = 1,   // pressure present; optional action
    Urgent = 2,   // soft threshold crossed
    Critical = 3  // life-threatening / action required
};

struct SurvivalDecision {
    SurvivalUrgency atmosphere{SurvivalUrgency::None}; // seek sealed air / ship life-support
    SurvivalUrgency shelter{SurvivalUrgency::None};    // leave hazard / find cover
    SurvivalUrgency energy{SurvivalUrgency::None};     // stop sprint / rest
    SurvivalUrgency food{SurvivalUrgency::None};       // eat / return to ship stores

    bool seekAtmosphere() const { return atmosphere != SurvivalUrgency::None; }
    bool seekShelter() const { return shelter != SurvivalUrgency::None; }
    bool conserveEnergy() const { return energy != SurvivalUrgency::None; }
    bool seekFood() const { return food != SurvivalUrgency::None; }
    bool producesDecision() const {
        return seekAtmosphere() || seekShelter() || conserveEnergy() || seekFood();
    }
    bool lifeThreat() const {
        return atmosphere == SurvivalUrgency::Critical ||
               shelter == SurvivalUrgency::Critical ||
               food == SurvivalUrgency::Critical;
    }
};

std::string_view survivalUrgencyName(SurvivalUrgency urgency);

// Fail-closed decision table over current meters + env pressure scalars.
SurvivalDecision evaluateSurvivalPressure(const SurvivalMeters& meters,
                                          const SurvivalContext& context,
                                          const SurvivalRateTable& rates = survivalRateTable());

// Pure one-tick projection matching ECS vitals energy/hunger/O2/hazard math
// without linking EnTT. Headless tests use this to prove hunger/energy pressure.
SurvivalMeters projectSurvivalMeters(SurvivalMeters meters,
                                     const SurvivalContext& context,
                                     float dt,
                                     const SurvivalRateTable& rates = survivalRateTable());

// Minimal ration restore hook (no IndustryItemId — avoids MachineType/item churn).
// Clamps to [0,100]. Returns resulting hunger.
float applySurvivalRation(float hunger, float restoreAmount = 40.0f);

} // namespace elysium
