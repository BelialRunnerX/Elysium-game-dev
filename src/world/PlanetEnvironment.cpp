#include "world/PlanetEnvironment.hpp"

#include <algorithm>

namespace elysium {
namespace {

// Representative biome names match BiomeCatalog::defaultBiomeFor display names /
// Part24 HUD labels for Temperate/Barren/Scorched.
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
constexpr PlanetEnvironment kFrozen{
    "Frozen", "Ice Sheet", 0.35f, 0.45f,
    {168, 196, 220, 255}, {210, 224, 236, 255}
};
constexpr PlanetEnvironment kToxic{
    "Toxic", "Spore Deeps", 1.2f, 0.85f,
    {72, 96, 58, 255}, {140, 158, 90, 255}
};
constexpr PlanetEnvironment kIrradiated{
    "Irradiated", "Glass Sea", 0.55f, 1.05f,
    {48, 62, 70, 255}, {180, 210, 190, 255}
};
constexpr PlanetEnvironment kOceanic{
    "Oceanic", "Kelp Shelf", 0.95f, 0.25f,
    {28, 78, 120, 255}, {70, 140, 150, 255}
};
constexpr PlanetEnvironment kAnomalous{
    "Anomalous", "Folded Mesa", 0.4f, 0.5f,
    {70, 58, 96, 255}, {150, 130, 170, 255}
};

constexpr float kSealedAtmosphereRefillPerSecond = -7.0f;

} // namespace

const PlanetEnvironment& planetEnvironmentFor(PlanetClass planetClass) {
    switch (planetClass) {
        case PlanetClass::Temperate: return kTemperate;
        case PlanetClass::Barren: return kBarren;
        case PlanetClass::Scorched: return kScorched;
        case PlanetClass::Frozen: return kFrozen;
        case PlanetClass::Toxic: return kToxic;
        case PlanetClass::Irradiated: return kIrradiated;
        case PlanetClass::Oceanic: return kOceanic;
        case PlanetClass::Anomalous: return kAnomalous;
        case PlanetClass::Count: break;
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
