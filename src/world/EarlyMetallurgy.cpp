#include "world/EarlyMetallurgy.hpp"

#include <algorithm>

namespace elysium {

int EarlyMetallurgy::clampTier(int tier) {
    return std::clamp(tier, kMinPersisted, kMaxPersisted);
}

std::string_view EarlyMetallurgy::headLabel(int tier) {
    const int t = clampTier(tier);
    if (t >= kSteel) return "Steel mining head";
    if (t >= kBronze) return "Bronze mining head";
    if (t >= kStone) return "Stone mining head";
    return "Improvised tools";
}

bool EarlyMetallurgy::canHarvest(int toolTier, int harvestTier) {
    return clampTier(toolTier) >= harvestTier;
}

EarlyMetallurgy::Eval EarlyMetallurgy::evaluate(int currentTier,
                                                HeadKind kind,
                                                int stoneCount,
                                                int copperOreCount,
                                                int tinOreCount,
                                                int bronzeIngotCount,
                                                int ironOreCount,
                                                int coalOreCount,
                                                int steelIngotCount) {
    const int tier = clampTier(currentTier);
    Eval out{};

    switch (kind) {
    case HeadKind::Stone:
        if (tier >= kStone) {
            out.message = "Stone mining head already installed.";
            return out;
        }
        if (stoneCount < kStoneCost) {
            out.message = "Need 4 Stone for a stone mining head.";
            return out;
        }
        out.canApply = true;
        out.nextTier = kStone;
        out.cost = {CostMode::StoneBlocks, kStoneCost, 0};
        out.message = "Stone mining head installed // Tier 1";
        return out;

    case HeadKind::Bronze:
        if (tier >= kBronze) {
            out.message = "Bronze mining head already installed.";
            return out;
        }
        if (tier < kStone) {
            out.message = "Need stone mining head before bronze.";
            return out;
        }
        if (bronzeIngotCount >= kBronzeIngotCost) {
            out.canApply = true;
            out.nextTier = kBronze;
            out.cost = {CostMode::BronzeIngots, kBronzeIngotCost, 0};
            out.message = "Bronze mining head installed // Tier 2 (alloy)";
            return out;
        }
        if (copperOreCount >= kBronzeCopperOreCost && tinOreCount >= kBronzeTinOreCost) {
            out.canApply = true;
            out.nextTier = kBronze;
            out.cost = {CostMode::BronzeOres, kBronzeCopperOreCost, kBronzeTinOreCost};
            out.message = "Bronze mining head installed // Tier 2 (ore shortcut)";
            return out;
        }
        out.message = "Need 4 Bronze Ingot (or 3 Copper Ore + 1 Tin Ore).";
        return out;

    case HeadKind::Steel:
        if (tier >= kSteel) {
            out.message = "Steel mining head already installed.";
            return out;
        }
        if (tier < kBronze) {
            out.message = "Need bronze mining head before steel.";
            return out;
        }
        if (steelIngotCount >= kSteelIngotCost) {
            out.canApply = true;
            out.nextTier = kSteel;
            out.cost = {CostMode::SteelIngots, kSteelIngotCost, 0};
            out.message = "Steel mining head installed // Tier 3 (alloy)";
            return out;
        }
        if (ironOreCount >= kSteelIronOreCost && coalOreCount >= kSteelCoalOreCost) {
            out.canApply = true;
            out.nextTier = kSteel;
            out.cost = {CostMode::SteelOres, kSteelIronOreCost, kSteelCoalOreCost};
            out.message = "Steel mining head installed // Tier 3 (ore shortcut)";
            return out;
        }
        out.message = "Need Tier 2 + 2 Steel Ingot (or 4 Iron Ore + 2 Coal Ore).";
        return out;
    }

    out.message = "Unknown mining-head upgrade.";
    return out;
}

} // namespace elysium
