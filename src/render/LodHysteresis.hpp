#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace elysium {

// Runtime representation tier for one cube-sphere surface chunk.
// Field is a cheap direct-field silhouette packet; Full is the editable
// macro/micro voxel packet. Only a bounded set of chunks around the player need
// Full residency during surface play.
enum class SurfaceRenderDetail : std::uint8_t { FieldFar = 0, FieldNear = 1, Full = 2 };

// P0-29 LOD hysteresis. Owned by PlanetSurfaceRenderer streaming focus.
// SurfaceChunkCache does not choose LOD; it only reconstructs LOD0 packets for
// addresses the renderer marks Visible / Prefetch / EditRemesh.
//
// Score is cosine similarity of chunk-center direction vs focus direction
// (1 = underfoot, -1 = antipode). Higher is nearer. Nominal enter cutoffs are
// the Nth / (N+M)th ranked scores for the Full and FieldNear budgets.
//
// Schmitt-trigger bands (asymmetric enter / exit):
//   Enter Full:      rank < highDetailBudget
//   Stay Full:       previous Full AND (score >= Nth - kLodFullExitBand
//                    OR rank < highDetailBudget + kLodHysteresisRankSlack)
//   Enter FieldNear: rank < highDetailBudget + nearFieldBudget (and not Full)
//   Stay FieldNear:  previous Full/Near AND (score >= (N+M)th - kLodNearExitBand
//                    OR rank < N+M + kLodHysteresisRankSlack)
//
// Widths are cosine units on the focus score, plus one extra rank of slack so
// the Nth / (N+1)th pair cannot swap tiers when the camera jitters across the
// budget edge. First assignment after entering streaming (or changing budgets)
// uses enter thresholds only so the residency bubble matches the request.
//
// kLodFocusDirtyCosine is a separate dirty-bit deadzone on setStreamingFocus
// (do not re-rank until focus moves ~1.8°). It is not an LOD enter/exit band.
inline constexpr float kLodFullExitBand = 0.03f;
inline constexpr float kLodNearExitBand = 0.03f;
inline constexpr int kLodHysteresisRankSlack = 1;
inline constexpr float kLodFocusDirtyCosine = 0.9995f;

struct LodHysteresisConfig {
    int highDetailBudget = 0;
    int nearFieldBudget = 0;
    float fullExitBand = kLodFullExitBand;
    float nearExitBand = kLodNearExitBand;
    int rankSlack = kLodHysteresisRankSlack;
};

// previous == nullptr treats every slot as FieldFar (enter-only assignment).
// previous and out may alias; each slot is read before it is written.
inline void assignStreamingLodDetails(const float* scores,
                                      const SurfaceRenderDetail* previous,
                                      SurfaceRenderDetail* out,
                                      int count,
                                      const LodHysteresisConfig& cfg) {
    if (!scores || !out || count <= 0) return;

    struct Ranked {
        float score{};
        int slot{};
    };
    std::vector<Ranked> ranked;
    ranked.reserve(static_cast<std::size_t>(count));
    for (int slot = 0; slot < count; ++slot)
        ranked.push_back({scores[slot], slot});
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.slot < b.slot;
    });

    const int fullBudget = std::clamp(cfg.highDetailBudget, 0, count);
    const int nearBudget = std::clamp(cfg.nearFieldBudget, 0, count - fullBudget);
    const int nearEnd = std::min(fullBudget + nearBudget, count);
    const int slack = std::max(0, cfg.rankSlack);

    const float fullEnter = fullBudget > 0
        ? ranked[static_cast<std::size_t>(fullBudget - 1)].score
        : std::numeric_limits<float>::infinity();
    const float nearEnter = nearBudget > 0
        ? ranked[static_cast<std::size_t>(nearEnd - 1)].score
        : std::numeric_limits<float>::infinity();
    const float fullExit = fullEnter - cfg.fullExitBand;
    const float nearExit = nearEnter - cfg.nearExitBand;

    std::vector<int> rankOf(static_cast<std::size_t>(count), count);
    for (int rank = 0; rank < count; ++rank)
        rankOf[static_cast<std::size_t>(ranked[static_cast<std::size_t>(rank)].slot)] = rank;

    for (int slot = 0; slot < count; ++slot) {
        const float score = scores[slot];
        const int rank = rankOf[static_cast<std::size_t>(slot)];
        const SurfaceRenderDetail prev = previous ? previous[slot]
                                                  : SurfaceRenderDetail::FieldFar;

        const bool enterFull = fullBudget > 0 && rank < fullBudget;
        const bool stayFull = prev == SurfaceRenderDetail::Full && fullBudget > 0 &&
                              (score >= fullExit || rank < fullBudget + slack);
        if (enterFull || stayFull) {
            out[slot] = SurfaceRenderDetail::Full;
            continue;
        }

        const bool enterNear = nearBudget > 0 && rank < nearEnd;
        const bool stayNear = (prev == SurfaceRenderDetail::FieldNear ||
                               prev == SurfaceRenderDetail::Full) &&
                              nearBudget > 0 &&
                              (score >= nearExit || rank < nearEnd + slack);
        if (enterNear || stayNear) {
            out[slot] = SurfaceRenderDetail::FieldNear;
            continue;
        }

        out[slot] = SurfaceRenderDetail::FieldFar;
    }
}

} // namespace elysium
