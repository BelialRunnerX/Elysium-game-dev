#pragma once

#include "core/JobSystem.hpp"
#include "core/Math.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace elysium {

struct PlayerInput {
    float moveForward{};
    float moveRight{};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    bool jump{};
    bool sprint{};
};

struct PlayerSnapshot {
    Vec3 position{};
    Vec3 velocity{};
    float health{100.0f};
    float oxygen{100.0f};
    float energy{100.0f};
    float hunger{100.0f};
    bool grounded{};
};

enum class ActorDomain : std::uint8_t { Planar = 0, PlanetSurface = 1 };

enum class EnemyArchetype : std::uint8_t {
    Drone = 0,
    Lictor = 1,
    Adept = 2,
    Praetor = 3
};

const char* enemyArchetypeName(EnemyArchetype archetype);

struct EnemySnapshot {
    std::uint64_t stableId{};
    Vec3 position{};
    float health{};
    ActorDomain domain{ActorDomain::Planar};
    EnemyArchetype archetype{EnemyArchetype::Drone};
};

struct PlayerPersistentState {
    Vec3 position{};
    float health{100.0f};
    float oxygen{100.0f};
    float energy{100.0f};
    float hunger{100.0f};
    std::vector<std::pair<int,int>> inventory;
};

struct RayAttackResult {
    bool hit{};
    bool killed{};
    std::uint64_t stableId{};
    float distance{};
};

struct SurfaceAiTelemetry {
    int sensed{};
    int thought{};
    int pathRequested{};
    int pathResolved{};
    int committed{};
    int attacks{};
    float attackDamageAttempted{};
    float attackDamageApplied{};
    int supportActions{};
    float supportHealing{};
};

struct EcsCommandTelemetry {
    int queued{};
    int spawned{};
    int damaged{};
    int destroyed{};
};

class EcsWorld {
public:
    using SolidQuery = std::function<bool(int,int,int)>;
    using SurfaceHoverQuery = std::function<Vec3(Vec3)>;
    using SurfacePathQuery = std::function<Vec3(Vec3,Vec3,float)>;
    using PlayerDamageFilter = std::function<float(Vec3,float)>;

    EcsWorld();
    ~EcsWorld();
    EcsWorld(EcsWorld&&) noexcept;
    EcsWorld& operator=(EcsWorld&&) noexcept;
    EcsWorld(const EcsWorld&) = delete;
    EcsWorld& operator=(const EcsWorld&) = delete;

    void createPlayer(Vec3 position);
    void setPlayerPosition(Vec3 position);
    void update(float dt,
                const PlayerInput& input,
                const SolidQuery& isSolid,
                float oxygenDrainPerSecond,
                float hazardDamagePerSecond);

    void updateVitals(float dt, float oxygenDrainPerSecond, float hazardDamagePerSecond, bool sprinting = false, bool moving = false);

    // Surface actors are kept in the same EnTT registry but tagged with an
    // explicit coordinate domain. Drones chase in the local tangent plane and
    // the caller projects their candidate position back to a stable hover
    // altitude on the cube-sphere world.
    void updateSurfaceEnemies(float dt, Vec3 playerCenter, const SurfaceHoverQuery& projectHover);
    SurfaceAiTelemetry updateSurfaceEnemiesPhased(JobSystem& jobs, float dt, Vec3 playerCenter,
                                                   const SurfaceHoverQuery& projectHover,
                                                   const SurfacePathQuery& resolvePath = {},
                                                   const PlayerDamageFilter& mitigatePlayerDamage = {});

    PlayerSnapshot player() const;
    PlayerPersistentState savePlayer() const;
    void loadPlayer(const PlayerPersistentState& state);

    void spawnEnemy(Vec3 position, EnemyArchetype archetype,
                    std::uint64_t stableId = 0, ActorDomain domain = ActorDomain::Planar);
    void queueSpawnEnemy(Vec3 position, EnemyArchetype archetype,
                         std::uint64_t stableId = 0, ActorDomain domain = ActorDomain::Planar);
    void spawnDrone(Vec3 position, std::uint64_t stableId = 0, ActorDomain domain = ActorDomain::Planar);
    void queueSpawnDrone(Vec3 position, std::uint64_t stableId = 0, ActorDomain domain = ActorDomain::Planar);
    void queueDestroyEnemy(std::uint64_t stableId);
    void queueDamageEnemy(std::uint64_t stableId, float damage);
    EcsCommandTelemetry flushCommands();
    int queuedCommandCount() const;
    void clearEnemies();
    std::vector<EnemySnapshot> enemies() const;
    std::vector<EnemySnapshot> enemies(ActorDomain domain) const;
    RayAttackResult attackRay(Vec3 origin, Vec3 direction, float maxDistance, float damage, ActorDomain domain = ActorDomain::Planar);

    int inventoryCount(int itemId) const;
    void addItem(int itemId, int count = 1);
    bool consumeItem(int itemId, int count = 1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace elysium
