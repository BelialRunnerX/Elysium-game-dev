#pragma once

#include "world/Block.hpp"
#include "world/PlanetTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace elysium {

// Stable biome content IDs. Append only; do not renumber mid-save once frozen.
enum class BiomeId : std::uint16_t {
    // Temperate
    TemperateFrontierPlains = 0,
    TemperateCoastalPlains,
    TemperateRollingHills,
    TemperateHighland,
    TemperateMountain,
    TemperateRiverValley,
    TemperateWetlandMarsh,
    TemperateBorealFringe,
    TemperateForest,
    TemperateScrubland,
    TemperateBadlands,
    TemperateKarst,
    // Barren
    BarrenRegolithPlain,
    BarrenImpactBasin,
    BarrenDustSea,
    BarrenSaltFlat,
    BarrenFractureMaze,
    BarrenCraterHighlands,
    BarrenShadowTrench,
    BarrenGlassEjecta,
    // Scorched
    ScorchedBasaltPlain,
    ScorchedObsidianRidge,
    ScorchedAshWaste,
    ScorchedCharForest,
    ScorchedMagmaField,
    ScorchedSulfurVentland,
    // Frozen
    FrozenIceSheet,
    FrozenPermafrostPlain,
    FrozenCryoRidge,
    FrozenSnowDune,
    FrozenGlacierForest,
    FrozenThermalRefuge,
    // Toxic
    ToxicSporeDeeps,
    ToxicFungalCanopy,
    ToxicAcidBog,
    ToxicCausticKarst,
    ToxicMiasmaForest,
    ToxicResinFlats,
    // Irradiated
    IrradiatedGlassSea,
    IrradiatedReactorBadlands,
    IrradiatedRadiantCrater,
    IrradiatedUranicRidge,
    IrradiatedDeadCity,
    IrradiatedAshenPlain,
    // Oceanic
    OceanicArchipelago,
    OceanicStormCoast,
    OceanicKelpShelf,
    OceanicReefGarden,
    OceanicAbyssalTrench,
    OceanicHydrothermalField,
    // Anomalous
    AnomalousFoldedMesa,
    AnomalousReverseCavern,
    AnomalousFloatingShelf,
    AnomalousGravityScar,
    AnomalousMirrorWaste,
    AnomalousImpossibleReef,
    Count
};

constexpr int kBiomeIdCount = static_cast<int>(BiomeId::Count);

// EnTT-free content row. Tags are query keys (comma-free tokens in the span).
// Surface/subsoil hints use existing BlockType only — no new blocks invented here.
struct BiomeDef {
    BiomeId id{};
    std::string_view name;
    PlanetClass planetClass{};
    std::span<const std::string_view> tags;
    BlockType surfaceHint{BlockType::Stone};
    BlockType subsoilHint{BlockType::Stone};
};

std::span<const BiomeDef> allBiomes();
std::span<const BiomeDef> biomesFor(PlanetClass planetClass);
const BiomeDef* biomeDef(BiomeId id);
std::size_t biomeCountFor(PlanetClass planetClass);

// Representative biome for HUD / PlanetEnvironment.biome continuity.
BiomeId defaultBiomeFor(PlanetClass planetClass);

} // namespace elysium
