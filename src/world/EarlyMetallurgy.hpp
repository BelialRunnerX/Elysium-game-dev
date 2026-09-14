#pragma once

#include <string_view>

namespace elysium {

// Part 24 early metallurgy / mining-head ladder (persisted as Game::toolTier_).
// Numeric values match BlockProperties::harvestTier gates:
//   0 Improvised — soft surface only (harvest 0)
//   1 Stone      — stone / coal / copper / tin (harvest 1)
//   2 Bronze     — iron / basalt / doors (harvest 2)
//   3 Steel      — steel plate / beacon / airlock (harvest 3)
// Saves already storing 1/2/3 keep those meanings (stone/bronze/steel). New
// games start at 0 so the stone craft is an explicit first link.
struct EarlyMetallurgy {
    static constexpr int kImprovised = 0;
    static constexpr int kStone = 1;
    static constexpr int kBronze = 2;
    static constexpr int kSteel = 3;
    static constexpr int kMinPersisted = 0;
    static constexpr int kMaxPersisted = 5;

    static constexpr int kStoneCost = 4;
    // One AlloyBronze crucible batch is 4 Bronze Ingots (3 Cu + 1 Sn).
    static constexpr int kBronzeIngotCost = 4;
    static constexpr int kBronzeCopperOreCost = 3;
    static constexpr int kBronzeTinOreCost = 1;
    // Two Steel Ingots ≈ one AlloySteel pair after Iron+Carbon smelting.
    static constexpr int kSteelIngotCost = 2;
    static constexpr int kSteelIronOreCost = 4;
    static constexpr int kSteelCoalOreCost = 2;

    enum class HeadKind { Stone, Bronze, Steel };

    enum class CostMode {
        None,
        StoneBlocks,
        BronzeIngots,
        BronzeOres,
        SteelIngots,
        SteelOres
    };

    struct Cost {
        CostMode mode{CostMode::None};
        int primaryCount{};
        int secondaryCount{};
    };

    struct Eval {
        bool canApply{};
        int nextTier{};
        Cost cost{};
        std::string_view message{};
    };

    static int clampTier(int tier);
    static std::string_view headLabel(int tier);
    static bool canHarvest(int toolTier, int harvestTier);

    // Inventory counts are raw stack sizes (block or industry item IDs resolved
    // by the caller). Prefer industry alloy outputs when present; ore shortcuts
    // remain for the pre-machine field path.
    static Eval evaluate(int currentTier,
                         HeadKind kind,
                         int stoneCount,
                         int copperOreCount,
                         int tinOreCount,
                         int bronzeIngotCount,
                         int ironOreCount,
                         int coalOreCount,
                         int steelIngotCount);
};

} // namespace elysium
