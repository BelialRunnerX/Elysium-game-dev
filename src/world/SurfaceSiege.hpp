#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elysium {

// The Third Edition defines Imperial attention as territorial and active from
// Suspicion 25. The 25/50/75 band split below is a prototype tuning policy,
// not a locked arithmetic rule. The important contract is that Marked claims
// produce three-wave Register Actions and Hunted claims produce five waves
// ending in a Praetor.
enum class ImperialAttentionBand : std::uint8_t {
    Quiet = 0,
    Noted = 1,
    Marked = 2,
    Hunted = 3
};

enum class ImperialEnforcementType : std::uint8_t {
    Patrol = 0,
    RegisterAction = 1
};

enum class RegisterActionPhase : std::uint8_t {
    Idle = 0,
    Announced = 1,
    WaveActive = 2,
    InterWave = 3,
    Cleared = 4,
    Failed = 5
};

enum class ImperialUnitRole : std::uint8_t {
    Drone = 0,
    Lictor = 1,
    Adept = 2,
    Praetor = 3
};

const char* imperialAttentionBandName(ImperialAttentionBand band);
const char* imperialEnforcementTypeName(ImperialEnforcementType type);
const char* registerActionPhaseName(RegisterActionPhase phase);
const char* imperialUnitRoleName(ImperialUnitRole role);

struct SurfaceSiegeTuning {
    float notedThreshold{25.0f};
    float markedThreshold{50.0f};
    float huntedThreshold{75.0f};
    // The specification describes Register Actions as announced about ten
    // minutes in advance. Tests can inject a shorter value without changing
    // production semantics.
    float registerAnnouncementSeconds{600.0f};
    float interWaveSeconds{18.0f};
};

struct RegisterActionSpawnRequest {
    std::uint64_t actionId{};
    std::uint64_t stableEnemyId{};
    ImperialUnitRole role{ImperialUnitRole::Drone};
    int waveIndex{};       // zero based
    int ordinal{};         // deterministic order inside the wave
    float azimuthRadians{};
    float distanceMeters{};
};

struct SurfaceSiegeState {
    std::uint64_t actionId{};
    ImperialEnforcementType type{ImperialEnforcementType::Patrol};
    ImperialAttentionBand band{ImperialAttentionBand::Quiet};
    RegisterActionPhase phase{RegisterActionPhase::Idle};
    int waveIndex{};
    int totalWaves{};
    float phaseSecondsRemaining{};
    float startingSuspicion{};
    bool claimRequired{};
    std::vector<std::uint64_t> activeEnemyIds;
};

struct SurfaceSiegeTelemetry {
    std::uint64_t actionsStarted{};
    std::uint64_t patrolsStarted{};
    std::uint64_t registerActionsStarted{};
    std::uint64_t wavesSpawned{};
    std::uint64_t enemiesRequested{};
    std::uint64_t enemiesReconciledDestroyed{};
    std::uint64_t actionsCleared{};
    std::uint64_t actionsFailed{};
};

// Deterministic, dependency-free Imperial enforcement director. It owns no ECS
// entities and no dense planetary state. Instead it emits stable-ID spawn
// requests that an owner-thread ECS command buffer can publish. This makes the
// director portable into the coworker's address-keyed running build without
// coupling it to EnTT or renderer/world implementations.
class SurfaceSiegeDirector {
public:
    explicit SurfaceSiegeDirector(std::uint64_t worldSeed = 0,
                                  SurfaceSiegeTuning tuning = {});

    ImperialAttentionBand bandFor(float suspicion) const;

    // Called after the normal deterministic dispatch roll succeeds. Unclaimed
    // systems and Noted claims receive a patrol. Marked/Hunted claims receive
    // a Register Action with the specification's 3/5-wave structure.
    bool requestEnforcement(float suspicion, bool claimed);

    // Advance announcement/inter-wave timing and emit the next wave when due.
    // `beaconIntact` is the claim objective seam: losing the Registry Beacon
    // fails an active Register Action but never erases constructed voxels.
    std::vector<RegisterActionSpawnRequest> update(float dt,
                                                   bool claimed,
                                                   bool beaconIntact);

    // Reconcile director-owned stable IDs against the ECS snapshot. This avoids
    // direct ECS callbacks from worker/combat systems and keeps kill publication
    // deterministic on the owner thread.
    void reconcileLiveEnemies(const std::vector<std::uint64_t>& liveStableIds);
    bool notifyEnemyDestroyed(std::uint64_t stableEnemyId);
    void notifyBeaconDestroyed();

    bool active() const;
    bool terminal() const;
    void clearTerminalState();

    const SurfaceSiegeState& state() const { return state_; }
    const SurfaceSiegeTelemetry& telemetry() const { return telemetry_; }
    const SurfaceSiegeTuning& tuning() const { return tuning_; }

    // Narrow state codec for save/merge integration. This is deliberately
    // independent of the prototype's global save schema and can be embedded as
    // a record in an authoritative campaign/system journal later.
    std::string serializeState() const;
    bool restoreState(std::string_view text, std::string* error = nullptr);

private:
    std::uint64_t worldSeed_{};
    std::uint64_t nextActionSerial_{1};
    SurfaceSiegeTuning tuning_{};
    SurfaceSiegeState state_{};
    SurfaceSiegeTelemetry telemetry_{};

    std::vector<ImperialUnitRole> compositionForCurrentWave() const;
    std::vector<RegisterActionSpawnRequest> spawnCurrentWave();
    std::uint64_t allocateActionId();
    std::uint64_t enemyStableId(int waveIndex, int ordinal, ImperialUnitRole role) const;
    void completeWaveIfEmpty();
    void failAction();
};

} // namespace elysium
