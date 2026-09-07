#pragma once

#include <cstdint>
#include <string_view>

namespace elysium {

struct SurvivalRateTable {
    float hungerDrainPerSecond{0.12f};
    float energySprintDrainPerSecond{13.0f};
    float energyRecoverPerSecond{8.0f};
    float asphyxiaDamagePerSecond{9.0f};
    float starvationDamagePerSecond{1.5f};
};

const SurvivalRateTable& survivalRateTable();

struct SurvivalMeters {
    float health{100.0f};
    float oxygen{100.0f};
    float energy{100.0f};
    float hunger{100.0f};
};

struct SurvivalContext {
    float effectiveOxygenDrainPerSecond{};
    float hazardDamagePerSecond{};
    bool sprinting{};
    bool moving{};
};

enum class SurvivalUrgency : std::uint8_t {
    None = 0,
    Advise = 1,
    Urgent = 2,
    Critical = 3
};

struct SurvivalDecision {
    SurvivalUrgency atmosphere{SurvivalUrgency::None};
    SurvivalUrgency shelter{SurvivalUrgency::None};
    SurvivalUrgency energy{SurvivalUrgency::None};
    SurvivalUrgency food{SurvivalUrgency::None};

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

SurvivalDecision evaluateSurvivalPressure(const SurvivalMeters& meters,
                                          const SurvivalContext& context,
                                          const SurvivalRateTable& rates = survivalRateTable());

SurvivalMeters projectSurvivalMeters(SurvivalMeters meters,
                                     const SurvivalContext& context,
                                     float dt,
                                     const SurvivalRateTable& rates = survivalRateTable());

float applySurvivalRation(float hunger, float restoreAmount = 40.0f);

} // namespace elysium
