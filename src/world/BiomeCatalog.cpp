#include "world/BiomeCatalog.hpp"

#include <array>
#include <iterator>

namespace elysium {
namespace {

// Tag storage — stable string literals; spans point here for the process lifetime.
constexpr std::string_view kTagOpen[]{"open", "agriculture", "landing"};
constexpr std::string_view kTagCoastal[]{"coastal", "landing", "salt"};
constexpr std::string_view kTagHills[]{"open", "agriculture", "traverse"};
constexpr std::string_view kTagHighland[]{"highland", "wind", "defense"};
constexpr std::string_view kTagMountain[]{"mountain", "vertical", "mining"};
constexpr std::string_view kTagRiver[]{"fertile", "flood", "logistics"};
constexpr std::string_view kTagWetland[]{"wetland", "biomass", "soft_foundation"};
constexpr std::string_view kTagBoreal[]{"forest", "cold_fringe", "timber"};
constexpr std::string_view kTagForest[]{"forest", "timber", "canopy"};
constexpr std::string_view kTagScrub[]{"scrub", "grazing", "fire_weather"};
constexpr std::string_view kTagBadlands[]{"arid", "specialty_geology", "dust"};
constexpr std::string_view kTagKarst[]{"cave", "unstable", "hidden_water"};

constexpr std::string_view kTagRegolith[]{"open", "dig_easy", "vacuum"};
constexpr std::string_view kTagBasin[]{"landing", "extraction", "exposed"};
constexpr std::string_view kTagDust[]{"dust", "mobility_tax", "filter"};
constexpr std::string_view kTagSalt[]{"traverse", "chemistry", "glare"};
constexpr std::string_view kTagFracture[]{"navigation", "radio", "ambush"};
constexpr std::string_view kTagCrater[]{"survey", "defense", "staging"};
constexpr std::string_view kTagShadow[]{"shelter", "vacuum", "cargo_hard"};
constexpr std::string_view kTagGlass[]{"salvage", "high_tier", "anomaly_geo"};

constexpr std::string_view kTagBasalt[]{"volcanic", "fortify", "heat"};
constexpr std::string_view kTagObsidian[]{"specialty_geology", "exposure"};
constexpr std::string_view kTagAsh[]{"ash", "visibility_tax", "filter"};
constexpr std::string_view kTagChar[]{"burned_organics", "fire_weather"};
constexpr std::string_view kTagMagma[]{"magma", "thermal", "plasma"};
constexpr std::string_view kTagSulfur[]{"vents", "corrosion", "chemistry"};

constexpr std::string_view kTagIce[]{"ice", "roads_easy", "thermal_cycle"};
constexpr std::string_view kTagPerma[]{"permafrost", "cold", "foundation_shift"};
constexpr std::string_view kTagCryo[]{"vertical", "ice_mining"};
constexpr std::string_view kTagSnow[]{"mobility_tax", "whiteout"};
constexpr std::string_view kTagGlacierForest[]{"sparse_organics", "timber_premium"};
constexpr std::string_view kTagRefuge[]{"oasis", "contested", "settlement"};

constexpr std::string_view kTagSpore[]{"neural", "organics", "filtration"};
constexpr std::string_view kTagFungal[]{"canopy", "organics", "filtration"};
constexpr std::string_view kTagAcid[]{"corrosion", "soft_foundation", "chemistry"};
constexpr std::string_view kTagCaustic[]{"karst", "corrosion", "specialty"};
constexpr std::string_view kTagMiasma[]{"biomass", "disease", "soft_foundation"};
constexpr std::string_view kTagResin[]{"resin", "biomass", "traverse"};

constexpr std::string_view kTagGlassSea[]{"salvage", "radiological", "high_tier"};
constexpr std::string_view kTagReactor[]{"salvage", "power_archaeology"};
constexpr std::string_view kTagRadiant[]{"survey", "radiation_schedule"};
constexpr std::string_view kTagUranic[]{"specialty_ore", "electronics_risk"};
constexpr std::string_view kTagDeadCity[]{"urban", "loot", "tombstone"};
constexpr std::string_view kTagAshen[]{"traverse", "low_organics", "ash"};

constexpr std::string_view kTagArch[]{"coastal", "staging", "ports"};
constexpr std::string_view kTagStormCoast[]{"coastal", "storm", "operations"};
constexpr std::string_view kTagKelp[]{"marine", "shelf", "resources"};
constexpr std::string_view kTagReef[]{"marine", "reef", "resources"};
constexpr std::string_view kTagAbyss[]{"marine", "deep_pressure", "specialty"};
constexpr std::string_view kTagHydro[]{"marine", "vents", "specialty"};

constexpr std::string_view kTagFolded[]{"anomaly", "navigation", "bounded"};
constexpr std::string_view kTagReverse[]{"anomaly", "cave", "bounded"};
constexpr std::string_view kTagFloat[]{"anomaly", "gravity", "bounded"};
constexpr std::string_view kTagGravity[]{"anomaly", "research", "hazard"};
constexpr std::string_view kTagMirror[]{"anomaly", "scanner", "chronicle"};
constexpr std::string_view kTagImpReef[]{"anomaly", "marine", "bounded"};

template <std::size_t N>
constexpr std::span<const std::string_view> tagsOf(const std::string_view (&arr)[N]) {
    return {arr, N};
}

constexpr BiomeDef kBiomes[]{
    // Temperate (12)
    {BiomeId::TemperateFrontierPlains, "Frontier Plains", PlanetClass::Temperate,
     tagsOf(kTagOpen), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateCoastalPlains, "Coastal Plains", PlanetClass::Temperate,
     tagsOf(kTagCoastal), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateRollingHills, "Rolling Hills", PlanetClass::Temperate,
     tagsOf(kTagHills), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateHighland, "Highland", PlanetClass::Temperate,
     tagsOf(kTagHighland), BlockType::Grass, BlockType::Stone},
    {BiomeId::TemperateMountain, "Mountain", PlanetClass::Temperate,
     tagsOf(kTagMountain), BlockType::Stone, BlockType::Stone},
    {BiomeId::TemperateRiverValley, "River Valley", PlanetClass::Temperate,
     tagsOf(kTagRiver), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateWetlandMarsh, "Wetland Marsh", PlanetClass::Temperate,
     tagsOf(kTagWetland), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::TemperateBorealFringe, "Boreal Fringe", PlanetClass::Temperate,
     tagsOf(kTagBoreal), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateForest, "Temperate Forest", PlanetClass::Temperate,
     tagsOf(kTagForest), BlockType::Grass, BlockType::Dirt},
    {BiomeId::TemperateScrubland, "Scrubland", PlanetClass::Temperate,
     tagsOf(kTagScrub), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::TemperateBadlands, "Badlands", PlanetClass::Temperate,
     tagsOf(kTagBadlands), BlockType::Stone, BlockType::Stone},
    {BiomeId::TemperateKarst, "Karst", PlanetClass::Temperate,
     tagsOf(kTagKarst), BlockType::Stone, BlockType::Stone},

    // Barren (8)
    {BiomeId::BarrenRegolithPlain, "Regolith Plain", PlanetClass::Barren,
     tagsOf(kTagRegolith), BlockType::Regolith, BlockType::Regolith},
    {BiomeId::BarrenImpactBasin, "Impact Basin", PlanetClass::Barren,
     tagsOf(kTagBasin), BlockType::Regolith, BlockType::Stone},
    {BiomeId::BarrenDustSea, "Dust Sea", PlanetClass::Barren,
     tagsOf(kTagDust), BlockType::Regolith, BlockType::Regolith},
    {BiomeId::BarrenSaltFlat, "Salt Flat", PlanetClass::Barren,
     tagsOf(kTagSalt), BlockType::Regolith, BlockType::Stone},
    {BiomeId::BarrenFractureMaze, "Fracture Maze", PlanetClass::Barren,
     tagsOf(kTagFracture), BlockType::Stone, BlockType::Stone},
    {BiomeId::BarrenCraterHighlands, "Crater Highlands", PlanetClass::Barren,
     tagsOf(kTagCrater), BlockType::Regolith, BlockType::Stone},
    {BiomeId::BarrenShadowTrench, "Shadow Trench", PlanetClass::Barren,
     tagsOf(kTagShadow), BlockType::Stone, BlockType::Stone},
    {BiomeId::BarrenGlassEjecta, "Glass Ejecta", PlanetClass::Barren,
     tagsOf(kTagGlass), BlockType::Stone, BlockType::Stone},

    // Scorched (6)
    {BiomeId::ScorchedBasaltPlain, "Basalt Plain", PlanetClass::Scorched,
     tagsOf(kTagBasalt), BlockType::Basalt, BlockType::Basalt},
    {BiomeId::ScorchedObsidianRidge, "Obsidian Ridge", PlanetClass::Scorched,
     tagsOf(kTagObsidian), BlockType::Basalt, BlockType::Stone},
    {BiomeId::ScorchedAshWaste, "Ash Waste", PlanetClass::Scorched,
     tagsOf(kTagAsh), BlockType::Regolith, BlockType::Basalt},
    {BiomeId::ScorchedCharForest, "Char Forest", PlanetClass::Scorched,
     tagsOf(kTagChar), BlockType::Basalt, BlockType::Dirt},
    {BiomeId::ScorchedMagmaField, "Magma Field", PlanetClass::Scorched,
     tagsOf(kTagMagma), BlockType::Magma, BlockType::Basalt},
    {BiomeId::ScorchedSulfurVentland, "Sulfur Ventland", PlanetClass::Scorched,
     tagsOf(kTagSulfur), BlockType::Basalt, BlockType::Stone},

    // Frozen (6)
    {BiomeId::FrozenIceSheet, "Ice Sheet", PlanetClass::Frozen,
     tagsOf(kTagIce), BlockType::Stone, BlockType::Stone},
    {BiomeId::FrozenPermafrostPlain, "Permafrost Plain", PlanetClass::Frozen,
     tagsOf(kTagPerma), BlockType::Dirt, BlockType::Stone},
    {BiomeId::FrozenCryoRidge, "Cryo Ridge", PlanetClass::Frozen,
     tagsOf(kTagCryo), BlockType::Stone, BlockType::Stone},
    {BiomeId::FrozenSnowDune, "Snow Dune", PlanetClass::Frozen,
     tagsOf(kTagSnow), BlockType::Regolith, BlockType::Stone},
    {BiomeId::FrozenGlacierForest, "Glacier Forest", PlanetClass::Frozen,
     tagsOf(kTagGlacierForest), BlockType::Dirt, BlockType::Stone},
    {BiomeId::FrozenThermalRefuge, "Thermal Refuge", PlanetClass::Frozen,
     tagsOf(kTagRefuge), BlockType::Dirt, BlockType::Stone},

    // Toxic (6)
    {BiomeId::ToxicSporeDeeps, "Spore Deeps", PlanetClass::Toxic,
     tagsOf(kTagSpore), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::ToxicFungalCanopy, "Fungal Canopy", PlanetClass::Toxic,
     tagsOf(kTagFungal), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::ToxicAcidBog, "Acid Bog", PlanetClass::Toxic,
     tagsOf(kTagAcid), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::ToxicCausticKarst, "Caustic Karst", PlanetClass::Toxic,
     tagsOf(kTagCaustic), BlockType::Stone, BlockType::Stone},
    {BiomeId::ToxicMiasmaForest, "Miasma Forest", PlanetClass::Toxic,
     tagsOf(kTagMiasma), BlockType::Dirt, BlockType::Dirt},
    {BiomeId::ToxicResinFlats, "Resin Flats", PlanetClass::Toxic,
     tagsOf(kTagResin), BlockType::Dirt, BlockType::Stone},

    // Irradiated (6)
    {BiomeId::IrradiatedGlassSea, "Glass Sea", PlanetClass::Irradiated,
     tagsOf(kTagGlassSea), BlockType::Stone, BlockType::Stone},
    {BiomeId::IrradiatedReactorBadlands, "Reactor Badlands", PlanetClass::Irradiated,
     tagsOf(kTagReactor), BlockType::Regolith, BlockType::Stone},
    {BiomeId::IrradiatedRadiantCrater, "Radiant Crater", PlanetClass::Irradiated,
     tagsOf(kTagRadiant), BlockType::Stone, BlockType::Stone},
    {BiomeId::IrradiatedUranicRidge, "Uranic Ridge", PlanetClass::Irradiated,
     tagsOf(kTagUranic), BlockType::Stone, BlockType::Stone},
    {BiomeId::IrradiatedDeadCity, "Dead City", PlanetClass::Irradiated,
     tagsOf(kTagDeadCity), BlockType::Stone, BlockType::Stone},
    {BiomeId::IrradiatedAshenPlain, "Ashen Plain", PlanetClass::Irradiated,
     tagsOf(kTagAshen), BlockType::Regolith, BlockType::Stone},

    // Oceanic (6)
    {BiomeId::OceanicArchipelago, "Archipelago", PlanetClass::Oceanic,
     tagsOf(kTagArch), BlockType::Grass, BlockType::Dirt},
    {BiomeId::OceanicStormCoast, "Storm Coast", PlanetClass::Oceanic,
     tagsOf(kTagStormCoast), BlockType::Stone, BlockType::Dirt},
    {BiomeId::OceanicKelpShelf, "Kelp Shelf", PlanetClass::Oceanic,
     tagsOf(kTagKelp), BlockType::Dirt, BlockType::Stone},
    {BiomeId::OceanicReefGarden, "Reef Garden", PlanetClass::Oceanic,
     tagsOf(kTagReef), BlockType::Stone, BlockType::Stone},
    {BiomeId::OceanicAbyssalTrench, "Abyssal Trench", PlanetClass::Oceanic,
     tagsOf(kTagAbyss), BlockType::Stone, BlockType::Stone},
    {BiomeId::OceanicHydrothermalField, "Hydrothermal Field", PlanetClass::Oceanic,
     tagsOf(kTagHydro), BlockType::Basalt, BlockType::Stone},

    // Anomalous (6)
    {BiomeId::AnomalousFoldedMesa, "Folded Mesa", PlanetClass::Anomalous,
     tagsOf(kTagFolded), BlockType::Stone, BlockType::Stone},
    {BiomeId::AnomalousReverseCavern, "Reverse Cavern", PlanetClass::Anomalous,
     tagsOf(kTagReverse), BlockType::Stone, BlockType::Dirt},
    {BiomeId::AnomalousFloatingShelf, "Floating Shelf", PlanetClass::Anomalous,
     tagsOf(kTagFloat), BlockType::Stone, BlockType::Stone},
    {BiomeId::AnomalousGravityScar, "Gravity Scar", PlanetClass::Anomalous,
     tagsOf(kTagGravity), BlockType::Basalt, BlockType::Stone},
    {BiomeId::AnomalousMirrorWaste, "Mirror Waste", PlanetClass::Anomalous,
     tagsOf(kTagMirror), BlockType::Regolith, BlockType::Stone},
    {BiomeId::AnomalousImpossibleReef, "Impossible Reef", PlanetClass::Anomalous,
     tagsOf(kTagImpReef), BlockType::Stone, BlockType::Dirt},
};

static_assert(std::size(kBiomes) == static_cast<std::size_t>(BiomeId::Count),
              "Biome catalog size must match BiomeId::Count");

} // namespace

std::span<const BiomeDef> allBiomes() {
    return {kBiomes, std::size(kBiomes)};
}

std::span<const BiomeDef> biomesFor(PlanetClass planetClass) {
    // Contiguous ranges match enum order in kBiomes.
    switch (planetClass) {
        case PlanetClass::Temperate: return {kBiomes + 0, 12};
        case PlanetClass::Barren: return {kBiomes + 12, 8};
        case PlanetClass::Scorched: return {kBiomes + 20, 6};
        case PlanetClass::Frozen: return {kBiomes + 26, 6};
        case PlanetClass::Toxic: return {kBiomes + 32, 6};
        case PlanetClass::Irradiated: return {kBiomes + 38, 6};
        case PlanetClass::Oceanic: return {kBiomes + 44, 6};
        case PlanetClass::Anomalous: return {kBiomes + 50, 6};
        case PlanetClass::Count: break;
    }
    return {};
}

const BiomeDef* biomeDef(BiomeId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= std::size(kBiomes)) return nullptr;
    return &kBiomes[index];
}

std::size_t biomeCountFor(PlanetClass planetClass) {
    return biomesFor(planetClass).size();
}

BiomeId defaultBiomeFor(PlanetClass planetClass) {
    switch (planetClass) {
        case PlanetClass::Temperate: return BiomeId::TemperateFrontierPlains;
        case PlanetClass::Barren: return BiomeId::BarrenRegolithPlain;
        case PlanetClass::Scorched: return BiomeId::ScorchedBasaltPlain;
        case PlanetClass::Frozen: return BiomeId::FrozenIceSheet;
        case PlanetClass::Toxic: return BiomeId::ToxicSporeDeeps;
        case PlanetClass::Irradiated: return BiomeId::IrradiatedGlassSea;
        case PlanetClass::Oceanic: return BiomeId::OceanicKelpShelf;
        case PlanetClass::Anomalous: return BiomeId::AnomalousFoldedMesa;
        case PlanetClass::Count: break;
    }
    return BiomeId::TemperateFrontierPlains;
}

} // namespace elysium
