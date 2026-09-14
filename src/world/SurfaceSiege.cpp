#include "world/SurfaceSiege.hpp"

#include "core/Determinism.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <ostream>
#include <sstream>
#include <tuple>

namespace elysium {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint64_t kActionLabel = 0x524547414354ULL; // "REGACT"
constexpr std::uint64_t kEnemyLabel = 0x454E464F524345ULL; // "ENFORCE"

bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

} // namespace

const char* imperialAttentionBandName(ImperialAttentionBand band) {
    switch (band) {
        case ImperialAttentionBand::Quiet: return "Quiet";
        case ImperialAttentionBand::Noted: return "Noted";
        case ImperialAttentionBand::Marked: return "Marked";
        case ImperialAttentionBand::Hunted: return "Hunted";
    }
    return "Unknown";
}

const char* imperialEnforcementTypeName(ImperialEnforcementType type) {
    switch (type) {
        case ImperialEnforcementType::Patrol: return "Patrol";
        case ImperialEnforcementType::RegisterAction: return "Register Action";
    }
    return "Unknown";
}

const char* registerActionPhaseName(RegisterActionPhase phase) {
    switch (phase) {
        case RegisterActionPhase::Idle: return "Idle";
        case RegisterActionPhase::Announced: return "Announced";
        case RegisterActionPhase::WaveActive: return "Wave Active";
        case RegisterActionPhase::InterWave: return "Inter-Wave";
        case RegisterActionPhase::Cleared: return "Cleared";
        case RegisterActionPhase::Failed: return "Failed";
    }
    return "Unknown";
}

const char* imperialUnitRoleName(ImperialUnitRole role) {
    switch (role) {
        case ImperialUnitRole::Drone: return "Drone";
        case ImperialUnitRole::Lictor: return "Lictor";
        case ImperialUnitRole::Adept: return "Adept";
        case ImperialUnitRole::Praetor: return "Praetor";
    }
    return "Unknown";
}

SurfaceSiegeDirector::SurfaceSiegeDirector(std::uint64_t worldSeed, SurfaceSiegeTuning tuning)
    : worldSeed_(worldSeed), tuning_(tuning) {
    tuning_.notedThreshold = std::clamp(tuning_.notedThreshold, 0.0f, 100.0f);
    tuning_.markedThreshold = std::clamp(tuning_.markedThreshold, tuning_.notedThreshold, 100.0f);
    tuning_.huntedThreshold = std::clamp(tuning_.huntedThreshold, tuning_.markedThreshold, 100.0f);
    tuning_.registerAnnouncementSeconds = std::max(0.0f, tuning_.registerAnnouncementSeconds);
    tuning_.interWaveSeconds = std::max(0.0f, tuning_.interWaveSeconds);
}

ImperialAttentionBand SurfaceSiegeDirector::bandFor(float suspicion) const {
    if (suspicion >= tuning_.huntedThreshold) return ImperialAttentionBand::Hunted;
    if (suspicion >= tuning_.markedThreshold) return ImperialAttentionBand::Marked;
    if (suspicion >= tuning_.notedThreshold) return ImperialAttentionBand::Noted;
    return ImperialAttentionBand::Quiet;
}

std::uint64_t SurfaceSiegeDirector::allocateActionId() {
    for (;;) {
        const auto id = mix64(worldSeed_ ^ kActionLabel ^ nextActionSerial_++);
        if (id != 0) return id;
    }
}

bool SurfaceSiegeDirector::requestEnforcement(float suspicion, bool claimed) {
    if (active() || terminal()) return false;
    const auto band = bandFor(suspicion);
    if (band == ImperialAttentionBand::Quiet) return false;

    state_ = {};
    state_.actionId = allocateActionId();
    state_.band = band;
    state_.startingSuspicion = std::clamp(suspicion, 0.0f, 100.0f);

    const bool registerAction = claimed && band >= ImperialAttentionBand::Marked;
    state_.type = registerAction ? ImperialEnforcementType::RegisterAction : ImperialEnforcementType::Patrol;
    state_.claimRequired = registerAction;
    state_.totalWaves = registerAction ? (band == ImperialAttentionBand::Hunted ? 5 : 3) : 1;
    state_.waveIndex = 0;
    state_.phase = registerAction ? RegisterActionPhase::Announced : RegisterActionPhase::InterWave;
    state_.phaseSecondsRemaining = registerAction ? tuning_.registerAnnouncementSeconds : 0.0f;

    ++telemetry_.actionsStarted;
    if (registerAction) ++telemetry_.registerActionsStarted;
    else ++telemetry_.patrolsStarted;
    return true;
}

std::vector<ImperialUnitRole> SurfaceSiegeDirector::compositionForCurrentWave() const {
    if (state_.type == ImperialEnforcementType::Patrol) {
        if (state_.band == ImperialAttentionBand::Hunted)
            return {ImperialUnitRole::Lictor, ImperialUnitRole::Lictor, ImperialUnitRole::Adept};
        if (state_.band == ImperialAttentionBand::Marked)
            return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Lictor};
        return {ImperialUnitRole::Drone, ImperialUnitRole::Drone};
    }

    if (state_.band == ImperialAttentionBand::Hunted) {
        switch (state_.waveIndex) {
            case 0: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone};
            case 1: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Lictor};
            case 2: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Lictor, ImperialUnitRole::Adept};
            case 3: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Lictor, ImperialUnitRole::Lictor, ImperialUnitRole::Adept};
            default: return {ImperialUnitRole::Praetor, ImperialUnitRole::Lictor, ImperialUnitRole::Lictor, ImperialUnitRole::Adept};
        }
    }

    // Marked Register Action: exactly three waves, escalating from drones into
    // elite + support pressure as described by the Third Edition encounter rules.
    switch (state_.waveIndex) {
        case 0: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone};
        case 1: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Adept};
        default: return {ImperialUnitRole::Drone, ImperialUnitRole::Drone, ImperialUnitRole::Lictor, ImperialUnitRole::Adept};
    }
}

std::uint64_t SurfaceSiegeDirector::enemyStableId(int waveIndex, int ordinal, ImperialUnitRole role) const {
    std::uint64_t h = worldSeed_ ^ kEnemyLabel ^ state_.actionId;
    h = mix64(h ^ (static_cast<std::uint64_t>(waveIndex + 1) * 0x9E3779B97F4A7C15ULL));
    h = mix64(h ^ (static_cast<std::uint64_t>(ordinal + 1) * 0xD1B54A32D192ED03ULL));
    h = mix64(h ^ (static_cast<std::uint64_t>(role) + 1ULL));
    return h == 0 ? 1 : h;
}

std::vector<RegisterActionSpawnRequest> SurfaceSiegeDirector::spawnCurrentWave() {
    std::vector<RegisterActionSpawnRequest> out;
    const auto roles = compositionForCurrentWave();
    out.reserve(roles.size());
    state_.activeEnemyIds.clear();
    state_.activeEnemyIds.reserve(roles.size());

    for (int ordinal = 0; ordinal < static_cast<int>(roles.size()); ++ordinal) {
        const auto role = roles[static_cast<std::size_t>(ordinal)];
        const auto id = enemyStableId(state_.waveIndex, ordinal, role);
        const float unit = hash01(id, state_.waveIndex, ordinal, static_cast<int>(role), 0x415A494DULL);
        RegisterActionSpawnRequest request{};
        request.actionId = state_.actionId;
        request.stableEnemyId = id;
        request.role = role;
        request.waveIndex = state_.waveIndex;
        request.ordinal = ordinal;
        request.azimuthRadians = unit * 2.0f * kPi;
        request.distanceMeters = 13.0f + 1.75f * static_cast<float>(ordinal) +
                                 (role == ImperialUnitRole::Praetor ? 5.0f : 0.0f);
        out.push_back(request);
        state_.activeEnemyIds.push_back(id);
    }

    state_.phase = RegisterActionPhase::WaveActive;
    state_.phaseSecondsRemaining = 0.0f;
    ++telemetry_.wavesSpawned;
    telemetry_.enemiesRequested += out.size();
    return out;
}

std::vector<RegisterActionSpawnRequest> SurfaceSiegeDirector::update(float dt,
                                                                     bool claimed,
                                                                     bool beaconIntact) {
    std::vector<RegisterActionSpawnRequest> out;
    dt = std::max(0.0f, dt);

    if (state_.phase == RegisterActionPhase::Idle ||
        state_.phase == RegisterActionPhase::Cleared ||
        state_.phase == RegisterActionPhase::Failed) return out;

    if (state_.claimRequired && (!claimed || !beaconIntact)) {
        failAction();
        return out;
    }

    if (state_.phase == RegisterActionPhase::WaveActive) {
        completeWaveIfEmpty();
        return out;
    }

    state_.phaseSecondsRemaining = std::max(0.0f, state_.phaseSecondsRemaining - dt);
    if (state_.phaseSecondsRemaining > 0.0f) return out;

    if (state_.phase == RegisterActionPhase::Announced || state_.phase == RegisterActionPhase::InterWave)
        return spawnCurrentWave();
    return out;
}

void SurfaceSiegeDirector::completeWaveIfEmpty() {
    if (state_.phase != RegisterActionPhase::WaveActive || !state_.activeEnemyIds.empty()) return;
    if (state_.waveIndex + 1 >= state_.totalWaves) {
        state_.phase = RegisterActionPhase::Cleared;
        state_.phaseSecondsRemaining = 0.0f;
        ++telemetry_.actionsCleared;
        return;
    }
    ++state_.waveIndex;
    state_.phase = RegisterActionPhase::InterWave;
    state_.phaseSecondsRemaining = tuning_.interWaveSeconds;
}

void SurfaceSiegeDirector::reconcileLiveEnemies(const std::vector<std::uint64_t>& liveStableIds) {
    if (state_.phase != RegisterActionPhase::WaveActive || state_.activeEnemyIds.empty()) return;
    std::vector<std::uint64_t> live = liveStableIds;
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());

    const auto before = state_.activeEnemyIds.size();
    state_.activeEnemyIds.erase(
        std::remove_if(state_.activeEnemyIds.begin(), state_.activeEnemyIds.end(), [&](std::uint64_t id) {
            return !std::binary_search(live.begin(), live.end(), id);
        }), state_.activeEnemyIds.end());
    telemetry_.enemiesReconciledDestroyed += before - state_.activeEnemyIds.size();
    completeWaveIfEmpty();
}

bool SurfaceSiegeDirector::notifyEnemyDestroyed(std::uint64_t stableEnemyId) {
    const auto it = std::find(state_.activeEnemyIds.begin(), state_.activeEnemyIds.end(), stableEnemyId);
    if (it == state_.activeEnemyIds.end()) return false;
    state_.activeEnemyIds.erase(it);
    ++telemetry_.enemiesReconciledDestroyed;
    completeWaveIfEmpty();
    return true;
}

void SurfaceSiegeDirector::failAction() {
    if (!active()) return;
    state_.phase = RegisterActionPhase::Failed;
    state_.phaseSecondsRemaining = 0.0f;
    ++telemetry_.actionsFailed;
}

void SurfaceSiegeDirector::notifyBeaconDestroyed() {
    if (state_.claimRequired) failAction();
}

bool SurfaceSiegeDirector::active() const {
    return state_.phase == RegisterActionPhase::Announced ||
           state_.phase == RegisterActionPhase::WaveActive ||
           state_.phase == RegisterActionPhase::InterWave;
}

bool SurfaceSiegeDirector::terminal() const {
    return state_.phase == RegisterActionPhase::Cleared || state_.phase == RegisterActionPhase::Failed;
}

void SurfaceSiegeDirector::clearTerminalState() {
    if (!terminal()) return;
    state_ = {};
}

std::string SurfaceSiegeDirector::serializeState() const {
    std::ostringstream out;
    out << "ELYSIUM_SURFACE_SIEGE 1\n";
    out << nextActionSerial_ << ' ' << state_.actionId << ' '
        << static_cast<int>(state_.type) << ' ' << static_cast<int>(state_.band) << ' '
        << static_cast<int>(state_.phase) << ' ' << state_.waveIndex << ' ' << state_.totalWaves << ' '
        << std::setprecision(9) << state_.phaseSecondsRemaining << ' ' << state_.startingSuspicion << ' '
        << (state_.claimRequired ? 1 : 0) << ' ' << state_.activeEnemyIds.size() << '\n';
    for (const auto id : state_.activeEnemyIds) out << id << '\n';
    return out.str();
}

bool SurfaceSiegeDirector::restoreState(std::string_view text, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };

    std::istringstream in{std::string(text)};
    std::string magic;
    int schema{};
    if (!(in >> magic >> schema) || magic != "ELYSIUM_SURFACE_SIEGE" || schema != 1)
        return fail("unsupported surface siege state header");

    std::uint64_t nextSerial{}, actionId{};
    int type{}, band{}, phase{}, waveIndex{}, totalWaves{}, claimRequired{};
    float phaseSeconds{}, startingSuspicion{};
    std::size_t activeCount{};
    if (!(in >> nextSerial >> actionId >> type >> band >> phase >> waveIndex >> totalWaves >>
          phaseSeconds >> startingSuspicion >> claimRequired >> activeCount))
        return fail("malformed surface siege state");

    if (nextSerial == 0 || type < 0 || type > 1 || band < 0 || band > 3 || phase < 0 || phase > 5 ||
        waveIndex < 0 || totalWaves < 0 || activeCount > 128 || !finiteNonNegative(phaseSeconds) ||
        !std::isfinite(startingSuspicion))
        return fail("surface siege state values out of range");

    SurfaceSiegeState restored{};
    restored.actionId = actionId;
    restored.type = static_cast<ImperialEnforcementType>(type);
    restored.band = static_cast<ImperialAttentionBand>(band);
    restored.phase = static_cast<RegisterActionPhase>(phase);
    restored.waveIndex = waveIndex;
    restored.totalWaves = totalWaves;
    restored.phaseSecondsRemaining = phaseSeconds;
    restored.startingSuspicion = std::clamp(startingSuspicion, 0.0f, 100.0f);
    restored.claimRequired = claimRequired != 0;
    restored.activeEnemyIds.reserve(activeCount);
    for (std::size_t i = 0; i < activeCount; ++i) {
        std::uint64_t id{};
        if (!(in >> id) || id == 0) return fail("malformed active enforcement stable ID");
        restored.activeEnemyIds.push_back(id);
    }

    auto ids = restored.activeEnemyIds;
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return fail("duplicate active enforcement stable ID");

    if (restored.phase == RegisterActionPhase::WaveActive && restored.activeEnemyIds.empty())
        return fail("wave-active siege state has no active enemies");
    if (restored.phase == RegisterActionPhase::Idle && restored.actionId != 0)
        return fail("idle siege state has a nonzero action ID");
    if (restored.type == ImperialEnforcementType::RegisterAction && restored.totalWaves != 3 && restored.totalWaves != 5)
        return fail("register action must contain three or five waves");
    if (restored.type == ImperialEnforcementType::Patrol && restored.totalWaves != 1 && restored.phase != RegisterActionPhase::Idle)
        return fail("patrol must contain exactly one wave");

    nextActionSerial_ = nextSerial;
    state_ = std::move(restored);
    if (error) error->clear();
    return true;
}


void appendCampaignSiegeEmbed(std::ostream& out,
                              const SurfaceSiegeDirector* const* directors,
                              int planetCount) {
    if (!directors || planetCount < 0) {
        out << "siege_count 0\n";
        return;
    }
    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(planetCount));
    for (int i = 0; i < planetCount; ++i) {
        if (directors[i] && campaignSiegeEmbedEligible(*directors[i]))
            indices.push_back(i);
    }
    out << "siege_count " << indices.size() << '\n';
    for (const int planetIndex : indices) {
        out << "siege " << planetIndex << '\n';
        out << directors[planetIndex]->serializeState();
    }
}

bool restoreCampaignSiegeEmbed(std::istream& in,
                               std::size_t siegeCount,
                               SurfaceSiegeDirector* const* directors,
                               int planetCount,
                               std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!directors || planetCount <= 0)
        return fail("campaign siege embed has no director array");
    if (siegeCount > static_cast<std::size_t>(planetCount))
        return fail("campaign siege_count exceeds planet count");

    std::vector<bool> seen(static_cast<std::size_t>(planetCount), false);
    for (std::size_t n = 0; n < siegeCount; ++n) {
        std::string tag;
        int planetIndex{};
        if (!(in >> tag >> planetIndex) || tag != "siege")
            return fail("malformed campaign siege header");
        if (planetIndex < 0 || planetIndex >= planetCount)
            return fail("campaign siege planetIndex out of range");
        if (seen[static_cast<std::size_t>(planetIndex)])
            return fail("duplicate campaign siege planetIndex");
        seen[static_cast<std::size_t>(planetIndex)] = true;
        if (!directors[planetIndex])
            return fail("campaign siege target director is null");

        // Rebuild the exact serializeState() text for restoreState().
        std::string magic;
        int schema{};
        if (!(in >> magic >> schema) || magic != "ELYSIUM_SURFACE_SIEGE" || schema != 1)
            return fail("unsupported embedded surface siege header");

        std::uint64_t nextSerial{}, actionId{};
        int type{}, band{}, phase{}, waveIndex{}, totalWaves{}, claimRequired{};
        float phaseSeconds{}, startingSuspicion{};
        std::size_t activeCount{};
        if (!(in >> nextSerial >> actionId >> type >> band >> phase >> waveIndex >> totalWaves >>
              phaseSeconds >> startingSuspicion >> claimRequired >> activeCount))
            return fail("malformed embedded surface siege state");

        std::ostringstream payload;
        payload << "ELYSIUM_SURFACE_SIEGE 1\n";
        payload << nextSerial << ' ' << actionId << ' ' << type << ' ' << band << ' ' << phase << ' '
                << waveIndex << ' ' << totalWaves << ' ' << std::setprecision(9) << phaseSeconds << ' '
                << startingSuspicion << ' ' << claimRequired << ' ' << activeCount << '\n';
        for (std::size_t i = 0; i < activeCount; ++i) {
            std::uint64_t id{};
            if (!(in >> id)) return fail("malformed embedded enforcement stable ID list");
            payload << id << '\n';
        }

        std::string restoreError;
        if (!directors[planetIndex]->restoreState(payload.str(), &restoreError))
            return fail(restoreError.empty() ? "embedded surface siege restore failed"
                                             : restoreError.c_str());
    }
    if (error) error->clear();
    return true;
}

void reconcileCampaignSiegeAfterRestore(SurfaceSiegeDirector& director) {
    if (director.state().phase != RegisterActionPhase::WaveActive) return;
    // Campaign load clears ECS enemies; missing stable IDs are treated as
    // destroyed so the director advances without changing ELYSIUM_SURFACE_SIEGE.
    director.reconcileLiveEnemies({});
}

} // namespace elysium
