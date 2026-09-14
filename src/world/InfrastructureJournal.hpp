#pragma once

#include "world/SurfaceInfrastructure.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace elysium {

// Merge-facing infrastructure persistence record. Unlike the legacy prototype
// sidecar codec, this type is keyed only by stable object identity + an
// authoritative SurfaceCellAddress. It intentionally has no dense planet index,
// FaceResolution, TotalCells, or chunk-slot dependency so it can be inserted
// into a large-planet sparse edit journal directly.
enum class InfrastructureRecordKind : std::uint8_t {
    Machine = 0,
    Portal = 1,
    AirlockAssembly = 2,
    AutomationRule = 3
};

enum class InfrastructureJournalOp : std::uint8_t {
    Upsert = 0,
    Tombstone = 1
};

// Cross-shard dependency locator. A dependency always names both the stable
// object identity and the authoritative address of the shard that owns it.
// This lets the migrated engine request another sparse edit-journal shard
// without inventing a global dense column index.
struct InfrastructureStableRef {
    InfrastructureRecordKind kind{InfrastructureRecordKind::Machine};
    std::uint64_t stableId{};
    SurfaceCellAddress ownerAddress{};

    friend bool operator==(const InfrastructureStableRef&, const InfrastructureStableRef&) = default;
};

using InfrastructureRecordPayload = std::variant<
    SurfaceMachineObject,
    SurfacePortalObject,
    SurfaceAirlockAssembly,
    SurfaceAutomationRule>;

// Schema history is intentionally independent of the standalone chunk codec:
// v1 stable objects; v2 cross-shard dependency locators; v3 process inventory/
// recipe state; v4 explicit logistics references/progress; v5 Extractor cycle
// progress. Readers retain v1-v4 compatibility.
struct InfrastructureJournalRecord {
    static constexpr std::uint32_t SchemaVersion = 5;
    static constexpr std::uint32_t OldestSupportedSchemaVersion = 1;

    std::uint32_t schemaVersion{SchemaVersion};
    InfrastructureJournalOp op{InfrastructureJournalOp::Upsert};
    InfrastructureRecordKind kind{InfrastructureRecordKind::Machine};
    std::uint64_t stableId{};

    // Spatial owner used by the edit journal to shard the record. This is an
    // address, not a dense column/cell index. For AutomationRule it is the
    // owning Logic Controller's address.
    SurfaceCellAddress ownerAddress{};

    // Schema v2+: explicit stable cross-shard dependencies. Airlock assemblies
    // reference their controller and both portals. Automation rules reference
    // controller/source/target. Machine and Portal records have no dependencies.
    std::vector<InfrastructureStableRef> dependencies;

    // Upserts carry one payload matching kind. Tombstones deliberately carry
    // no payload; stableId + ownerAddress are enough to erase the object while
    // leaving the voxel edit journal authoritative for geometry.
    std::optional<InfrastructureRecordPayload> payload;
};

struct InfrastructureJournalShard {
    SurfaceCellAddress ownerAddress{};
    std::vector<InfrastructureJournalRecord> records;
};

enum class InfrastructureCompactionPolicy : std::uint8_t {
    // Correct default for a delta journal. Final tombstones remain so removal
    // of deterministic/previously committed objects cannot be resurrected.
    KeepTombstones = 0,

    // Only valid when the caller explicitly compacts against a known-empty
    // infrastructure baseline. Never use this for an ordinary save journal.
    DropTombstonesAgainstEmptyBaseline = 1
};

const char* infrastructureRecordKindName(InfrastructureRecordKind kind);
const char* infrastructureJournalOpName(InfrastructureJournalOp op);

InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceMachineObject& object,
                                                       std::vector<InfrastructureStableRef> dependencies = {});
InfrastructureJournalRecord makeInfrastructureUpsert(const SurfacePortalObject& object);
InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceAirlockAssembly& object,
                                                       InfrastructureStableRef controller,
                                                       InfrastructureStableRef innerPortal,
                                                       InfrastructureStableRef outerPortal);
InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceAutomationRule& object,
                                                       SurfaceCellAddress controllerAddress,
                                                       InfrastructureStableRef controller,
                                                       InfrastructureStableRef source,
                                                       InfrastructureStableRef target);
InfrastructureJournalRecord makeInfrastructureTombstone(InfrastructureRecordKind kind,
                                                         std::uint64_t stableId,
                                                         SurfaceCellAddress ownerAddress);

// Validation is intentionally topology-size agnostic. It verifies stable IDs,
// kind/payload consistency, finite persistent state, and dependency record
// syntax, but never rejects an address because u/v exceeds the old 64x64
// prototype.
bool validateInfrastructureJournalRecord(const InfrastructureJournalRecord& record,
                                         std::string* error = nullptr);

// Deterministic text codec suitable for embedding as a record payload inside a
// coworker-owned edit journal. Schema v1 records remain readable; schema v2
// adds explicit dependency locators; schema v3 adds persistent industrial
// machine inventory/recipe/process state; schema v4 adds directed Conveyor/
// Sorter/Cargo Loader stable references and transfer progress; schema v5 adds
// persistent Extractor cycle progress and the expanded industrial item vocabulary. The outer journal remains responsible for
// its own transaction/checksum/generation policy.
std::string serializeInfrastructureJournalRecord(const InfrastructureJournalRecord& record);
std::optional<InfrastructureJournalRecord> deserializeInfrastructureJournalRecord(std::string_view text,
                                                                                   std::string* error = nullptr);

// Captures the current stable infrastructure state as address-keyed upserts.
// Records are sorted in dependency order and then by stable ID:
// Machine -> Portal -> AirlockAssembly -> AutomationRule. Schema-v2 dependency
// references are resolved from the authoritative object addresses at capture.
bool captureInfrastructureUpserts(const SurfaceInfrastructure& infrastructure,
                                  std::vector<InfrastructureJournalRecord>& out,
                                  std::string* error = nullptr);

// Validate a self-contained infrastructure snapshot before applying it. The
// function compacts latest-write-wins records first, then verifies every v2
// dependency resolves to a surviving upsert with the expected kind/address.
// This catches dangling cross-shard Airlock/Logic references before mutation.
bool validateInfrastructureSnapshotClosure(const std::vector<InfrastructureJournalRecord>& records,
                                            std::string* error = nullptr);

// Latest-write-wins compaction by (kind, stableId). Stable object ownership is
// immutable: moving the same ID to a different ownerAddress is rejected because
// a sharded edit journal would require an explicit old-shard tombstone + new ID
// or a higher-level migration transaction. Output order is deterministic.
bool compactInfrastructureJournal(const std::vector<InfrastructureJournalRecord>& records,
                                  std::vector<InfrastructureJournalRecord>& out,
                                  InfrastructureCompactionPolicy policy = InfrastructureCompactionPolicy::KeepTombstones,
                                  std::string* error = nullptr);

// Groups records by exact owner address without allocating any dense planet
// structure. The coworker build can map ownerAddress to its own edit-journal
// shard/chunk key. Shards and records are deterministically ordered.
bool shardInfrastructureRecordsByOwnerAddress(const std::vector<InfrastructureJournalRecord>& records,
                                              std::vector<InfrastructureJournalShard>& out,
                                              std::string* error = nullptr);

// Resolve the dependency frontier for a partial/sharded journal load. A
// dependency is satisfied when the exact stable kind/ID/address is already
// present in the live infrastructure set or survives as an upsert in the same
// record batch. Missing refs are deduplicated and deterministically sorted so
// the caller can request those sparse owner-address shards without any global
// planet index.
bool collectMissingInfrastructureDependencies(const SurfaceInfrastructure& infrastructure,
                                               const std::vector<InfrastructureJournalRecord>& records,
                                               std::vector<InfrastructureStableRef>& out,
                                               std::string* error = nullptr);

// Applies records using stable-object APIs. Upserts are dependency ordered;
// tombstones are reverse dependency ordered and idempotent. Portal tombstones
// never clear the voxel cell: geometry remains owned by the ordinary voxel edit
// journal. For a complete snapshot, call validateInfrastructureSnapshotClosure
// first so cross-shard dependencies are proven before mutation.
bool applyInfrastructureJournal(SurfaceInfrastructure& infrastructure,
                                PlanetSurface& planet,
                                const std::vector<InfrastructureJournalRecord>& records,
                                std::string* error = nullptr);

} // namespace elysium
