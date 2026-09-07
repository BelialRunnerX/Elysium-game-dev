#pragma once

#include "world/Block.hpp"
#include "world/PlanetTypes.hpp"

#include <string_view>

namespace elysium {

// Authoritative per-PlanetClass survival/hazard baseline. Shared by planar World
// and cube-sphere PlanetSurface so Barren/Scorched/Temperate drain is not Game-
// only magic on either spatial store.
struct PlanetEnvironment {
    std::string_view name;
    std::string_view biome;
    float oxygenDrainPerSecond;
    float hazardDamagePerSecond;
    Color4u skyColor;
    Color4u horizonColor;
};

const PlanetEnvironment& planetEnvironmentFor(PlanetClass planetClass);

// Narrow Sense-step blend input. Intentionally free of SurfaceInfrastructure /
// EnTT types so headless tests and both spatial stores can share one policy.
struct AtmosphereSupportSample {
    float pressure{};
    float oxygen{};

    bool breathable() const { return pressure >= 0.55f && oxygen >= 0.45f; }
};

// effectiveDrain = f(envBaseline, atmosphereSample). Negative values refill suit O2.
// ECS vitals receive only the resulting scalar (Commit); they never query planet/infra.
float effectiveOxygenDrain(float envBaselineDrainPerSecond,
                           const AtmosphereSupportSample& atmosphere);

// Planar compatibility: sealed room reported as a boolean oxygenatedAt() sample.
float effectiveOxygenDrain(float envBaselineDrainPerSecond, bool sealedBreathable);

} // namespace elysium
