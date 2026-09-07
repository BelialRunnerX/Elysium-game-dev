#include "world/SurvivalPressure.hpp"

#include <algorithm>

namespace elysium {
namespace {

constexpr SurvivalRateTable kRates{};

} // namespace

const SurvivalRateTable& survivalRateTable() { return kRates; }

std::string_view survivalUrgencyName(SurvivalUrgency urgency) {
    switch (urgency) {
        case SurvivalUrgency::None: return "None";
        case SurvivalUrgency::Advise: return "Advise";
        case SurvivalUrgency::Urgent: return "Urgent";
        case SurvivalUrgency::Critical: return "Critical";
    }
    return "None";
}

SurvivalDecision evaluateSurvivalPressure(const SurvivalMeters& meters,
                                          const SurvivalContext& context,
                                          const SurvivalRateTable& /*rates*/) {
    SurvivalDecision d{};

    if (context.effectiveOxygenDrainPerSecond > 0.0f) {
        d.atmosphere = SurvivalUrgency::Advise;
        if (meters.oxygen < 40.0f) d.atmosphere = SurvivalUrgency::Urgent;
        if (meters.oxygen < 15.0f) d.atmosphere = SurvivalUrgency::Critical;
    }
    if (meters.oxygen <= 0.0f) {
        d.atmosphere = SurvivalUrgency::Critical;
    }
    if (context.effectiveOxygenDrainPerSecond < 0.0f && meters.oxygen > 40.0f) {
        d.atmosphere = SurvivalUrgency::None;
    }

    if (context.hazardDamagePerSecond > 0.0f) {
        d.shelter = SurvivalUrgency::Advise;
        if (meters.health < 50.0f) d.shelter = SurvivalUrgency::Urgent;
        if (meters.health < 25.0f) d.shelter = SurvivalUrgency::Critical;
    }

    if (meters.energy <= 0.5f) {
        d.energy = SurvivalUrgency::Critical;
    } else if (meters.energy < 15.0f) {
        d.energy = SurvivalUrgency::Urgent;
    } else if (meters.energy < 50.0f && context.sprinting && context.moving) {
        d.energy = SurvivalUrgency::Advise;
    }

    if (meters.hunger <= 0.0f) {
        d.food = SurvivalUrgency::Critical;
    } else if (meters.hunger < 20.0f) {
        d.food = SurvivalUrgency::Urgent;
    } else if (meters.hunger < 50.0f) {
        d.food = SurvivalUrgency::Advise;
    }

    return d;
}

SurvivalMeters projectSurvivalMeters(SurvivalMeters meters,
                                     const SurvivalContext& context,
                                     float dt,
                                     const SurvivalRateTable& rates) {
    if (dt <= 0.0f) return meters;

    meters.oxygen = std::clamp(
        meters.oxygen - context.effectiveOxygenDrainPerSecond * dt, 0.0f, 100.0f);
    meters.hunger = std::clamp(
        meters.hunger - rates.hungerDrainPerSecond * dt, 0.0f, 100.0f);

    if (context.sprinting && context.moving && meters.energy > 0.5f) {
        meters.energy = std::max(0.0f, meters.energy - rates.energySprintDrainPerSecond * dt);
    } else {
        meters.energy = std::min(100.0f, meters.energy + rates.energyRecoverPerSecond * dt);
    }

    float damage = context.hazardDamagePerSecond;
    if (meters.oxygen <= 0.0f) damage += rates.asphyxiaDamagePerSecond;
    if (meters.hunger <= 0.0f) damage += rates.starvationDamagePerSecond;
    if (damage > 0.0f) {
        meters.health = std::max(0.0f, meters.health - damage * dt);
    }
    return meters;
}

float applySurvivalRation(float hunger, float restoreAmount) {
    return std::clamp(hunger + std::max(0.0f, restoreAmount), 0.0f, 100.0f);
}

} // namespace elysium
