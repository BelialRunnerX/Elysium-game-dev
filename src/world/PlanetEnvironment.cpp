#include "world/PlanetEnvironment.hpp"

#include <algorithm>

namespace elysium {
namespace {

constexpr PlanetEnvironment kTemperate{
    "Temperate", "Frontier Plains", 0.0f, 0.0f,
    {116, 169, 205, 255}, {194, 208, 192, 255}
};
constexpr PlanetEnvironment kBarren{
    "Barren", "Regolith Plain", 1.8f, 0.0f,
    {34, 40, 58, 255}, {126, 111, 103, 255}
};
constexpr PlanetEnvironment kScorched{
    "Scorched", "Basalt Plain", 0.65f, 0.7f,
    {79, 38, 39, 255}, {174, 91, 53, 255}
};

constexpr float kSealedAtmosphereRefillPerSecond = -7.0f;

} // namespace

const PlanetEnvironment& planetEnvironmentFor(PlanetClass planetClass) {
    switch (planetClass) {
        case PlanetClass::Temperate: return kTemperate;
        case PlanetClass::Barren: return kBarren;
        case PlanetClass::Scorched: return kScorched;
    }
    return kTemperate;
}

float effectiveOxygenDrain(float envBaselineDrainPerSecond,
                           const AtmosphereSupportSample& atmosphere) {
    if (atmosphere.breathable()) return kSealedAtmosphereRefillPerSecond;

    float drain = envBaselineDrainPerSecond;
    if (drain > 0.0f && (atmosphere.pressure > 0.0f || atmosphere.oxygen > 0.0f)) {
        const float pressureShare = std::clamp(atmosphere.pressure / 0.55f, 0.0f, 1.0f);
        const float oxygenShare = std::clamp(atmosphere.oxygen / 0.45f, 0.0f, 1.0f);
        const float partialSupport = std::min(pressureShare, oxygenShare);
        drain *= 1.0f - 0.75f * partialSupport;
    }
    return drain;
}

float effectiveOxygenDrain(float envBaselineDrainPerSecond, bool sealedBreathable) {
    AtmosphereSupportSample sample{};
    if (sealedBreathable) {
        sample.pressure = 1.0f;
        sample.oxygen = 1.0f;
    }
    return effectiveOxygenDrain(envBaselineDrainPerSecond, sample);
}

} // namespace elysium
