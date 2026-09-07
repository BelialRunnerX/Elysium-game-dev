#pragma once

#include "world/PlanetSurface.hpp"
#include "world/SurfaceInfrastructure.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elysium {

// Sparse persistent record for one authoritative cube-sphere chunk. This is
// intentionally independent of EnTT entity IDs and GPU resources.
struct SurfaceChunkRecord {
    int planetSlot{};
    std::uint64_t planetSeed{};
    int generatorVersion{1};
    std::uint64_t generatorFingerprint{};
    // Whole-save commit generation. Generation 0 is reserved for legacy v7
    // sidecars that predate cross-file commit binding.
    std::uint64_t saveGeneration{};
    PlanetChunkAddress address{};
    SurfaceChunkJournal journal{};
    std::vector<SurfaceMachineObject> machines;
    std::vector<SurfacePortalObject> portals;
    std::vector<SurfaceAirlockAssembly> airlocks;
    std::vector<SurfaceAutomationRule> automationRules;
};

struct SurfaceChunkLoadResult {
    bool loaded{};
    bool usedPreviousGeneration{};
    std::string error;
    SurfaceChunkRecord record{};
};

// Per-touched-chunk transactional store. Files are written as a complete new
// generation to a temp path, durably flushed where the platform API is
// available, then atomically published. The previous known-good generation is
// retained as a fallback when the current file is corrupt or torn.
class SurfaceChunkStore {
public:
    static constexpr int RecordFormatVersion = 8;

    SurfaceChunkStore(std::filesystem::path root,
                      int planetSlot,
                      std::uint64_t planetSeed,
                      int generatorVersion,
                      std::uint64_t generatorFingerprint);

    bool save(const SurfaceChunkRecord& record, std::string* error = nullptr) const;
    SurfaceChunkLoadResult load(const PlanetChunkAddress& address, std::optional<std::uint64_t> expectedSaveGeneration = std::nullopt) const;
    bool remove(const PlanetChunkAddress& address, std::string* error = nullptr) const;

    std::filesystem::path recordPath(const PlanetChunkAddress& address) const;
    std::filesystem::path previousPath(const PlanetChunkAddress& address) const;

    int planetSlot() const { return planetSlot_; }
    std::uint64_t planetSeed() const { return planetSeed_; }
    int generatorVersion() const { return generatorVersion_; }
    std::uint64_t generatorFingerprint() const { return generatorFingerprint_; }

private:
    std::filesystem::path root_;
    int planetSlot_{};
    std::uint64_t planetSeed_{};
    int generatorVersion_{};
    std::uint64_t generatorFingerprint_{};

    SurfaceChunkLoadResult loadPath(const std::filesystem::path& path,
                                    const PlanetChunkAddress& expected,
                                    std::optional<std::uint64_t> expectedSaveGeneration) const;
};

// v0.11 generator compatibility contract. This fingerprint is save metadata,
// not a gameplay random seed. Any baseline-generation change must intentionally
// bump the version/fingerprint rather than silently reinterpreting old deltas.
constexpr int kSurfaceGeneratorVersion = PlanetSurface::GeneratorVersion;
constexpr std::uint64_t kSurfaceGeneratorFingerprint = PlanetSurface::GeneratorFingerprint;


// Generic complete-generation publisher used by the v7 global manifest as well
// as chunk sidecars. The destination remains plain caller-provided bytes; the
// helper only provides temp-write/durable-flush/atomic-replace + .prev rotation.
bool writeAtomicGeneration(const std::filesystem::path& destination,
                           std::string_view bytes,
                           std::string* error = nullptr);


struct CheckedGenerationReadResult {
    bool loaded{};
    bool usedPreviousGeneration{};
    std::string data;
    std::string error;
};

// Checksum-wrapped atomic generation used by the global v8 manifest. When the
// current generation is torn/corrupt, read attempts the .prev generation.
// allowPlainLegacy keeps v1-v6 human-readable manifests loadable.
bool writeCheckedAtomicGeneration(const std::filesystem::path& destination,
                                  std::string_view payload,
                                  std::string* error = nullptr);
CheckedGenerationReadResult readCheckedAtomicGeneration(const std::filesystem::path& destination,
                                                        bool allowPlainLegacy = true);

struct SurfaceManifestChunkRef {
    int planetSlot{};
    PlanetChunkAddress address{};
};

// Extracts the sidecar references from v7+ global manifests without applying
// gameplay state. Used by save cleanup so the current and previous manifest
// generations retain exactly the sidecars they may need for recovery.
std::vector<SurfaceManifestChunkRef> extractSurfaceManifestChunkRefs(std::string_view manifestPayload);

// Removes stale .tmp files and sidecar generations that are referenced by
// neither the newly committed manifest nor its retained previous manifest.
// This deliberately keeps the previous manifest's recoverability intact.
bool pruneSurfaceChunkStore(const std::filesystem::path& root,
                            const std::vector<SurfaceManifestChunkRef>& currentRefs,
                            const std::vector<SurfaceManifestChunkRef>& previousRefs,
                            std::string* error = nullptr);

SurfaceChunkRecord makeSurfaceChunkRecord(int planetSlot,
                                          const PlanetSurface& surface,
                                          const SurfaceInfrastructure& infrastructure,
                                          const PlanetChunkAddress& address,
                                          std::uint64_t saveGeneration = 0);

} // namespace elysium
