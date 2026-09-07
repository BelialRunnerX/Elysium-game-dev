#pragma once

#include <string_view>

namespace elysium {

struct EarlyMetallurgy {
    static constexpr int kImprovised = 0;
    static constexpr int kStone = 1;
    static constexpr int kBronze = 2;
    static constexpr int kSteel = 3;
    static constexpr int kMinPersisted = 0;
    static constexpr int kMaxPersisted = 5;

    static constexpr int kStoneCost = 4;
    static constexpr int kBronzeIngotCost = 4;
    static constexpr int kBronzeCopperOreCost = 3;
    static constexpr int kBronzeTinOreCost = 1;
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
