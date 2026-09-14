#include "ecs/EcsWorld.hpp"

#include <entt/entt.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace elysium {
namespace {

struct Position { Vec3 value{}; };
struct Velocity { Vec3 value{}; };
struct PlayerTag {};
struct EnemyTag {};
struct StableId { std::uint64_t value{}; };
struct Health { float current{100.0f}; float max{100.0f}; };
struct Survival { float oxygen{100.0f}; float energy{100.0f}; float hunger{100.0f}; };
struct Grounded { bool value{}; };
struct DroneAI { float attackCooldown{}; };
struct EnemyKind { EnemyArchetype value{EnemyArchetype::Drone}; };
struct EnemyCombatProfile {
    float moveSpeed{3.2f};
    float attackDamage{8.0f};
    float attackRange{2.35f};
    float attackPeriod{1.1f};
    float supportRange{};
    float supportHeal{};
};
struct CoordinateDomain { ActorDomain value{ActorDomain::Planar}; };
struct Inventory { std::unordered_map<int,int> items; };

enum class EcsCommandType : std::uint8_t { SpawnDrone = 0, DestroyEnemy = 1, DamageEnemy = 2 };
struct PendingEcsCommand {
    EcsCommandType type{EcsCommandType::SpawnDrone};
    std::uint64_t sequence{};
    Vec3 position{};
    std::uint64_t stableId{};
    ActorDomain domain{ActorDomain::Planar};
    EnemyArchetype archetype{EnemyArchetype::Drone};
    float amount{};
};


struct EnemyProfileDefinition {
    float health;
    EnemyCombatProfile combat;
};

EnemyProfileDefinition profileFor(EnemyArchetype archetype) {
    switch(archetype) {
        case EnemyArchetype::Drone: return {30.0f,{3.2f,8.0f,2.35f,1.1f,0.0f,0.0f}};
        case EnemyArchetype::Lictor: return {95.0f,{2.1f,14.0f,2.55f,1.35f,0.0f,0.0f}};
        case EnemyArchetype::Adept: return {48.0f,{2.55f,5.0f,2.25f,1.35f,8.0f,7.0f}};
        case EnemyArchetype::Praetor: return {260.0f,{2.35f,20.0f,2.8f,1.55f,0.0f,0.0f}};
    }
    return {30.0f,{}};
}

bool collides(const Vec3& feet, const EcsWorld::SolidQuery& isSolid) {
    constexpr float half = 0.29f;
    constexpr float height = 1.78f;
    constexpr float eps = 0.001f;
    const int minX = static_cast<int>(std::floor(feet.x - half));
    const int maxX = static_cast<int>(std::floor(feet.x + half - eps));
    const int minY = static_cast<int>(std::floor(feet.y));
    const int maxY = static_cast<int>(std::floor(feet.y + height - eps));
    const int minZ = static_cast<int>(std::floor(feet.z - half));
    const int maxZ = static_cast<int>(std::floor(feet.z + half - eps));

    for (int y = minY; y <= maxY; ++y)
        for (int z = minZ; z <= maxZ; ++z)
            for (int x = minX; x <= maxX; ++x)
                if (isSolid(x,y,z)) return true;
    return false;
}

float raySphere(Vec3 ro, Vec3 rd, Vec3 center, float radius) {
    const Vec3 oc = ro - center;
    const float b = dot(oc, rd);
    const float c = dot(oc, oc) - radius * radius;
    const float h = b*b - c;
    if (h < 0.0f) return std::numeric_limits<float>::infinity();
    const float s = std::sqrt(h);
    float t = -b - s;
    if (t < 0.0f) t = -b + s;
    return t >= 0.0f ? t : std::numeric_limits<float>::infinity();
}

// Metabolic rates must stay aligned with world/SurvivalPressure::survivalRateTable()
// (ecs does not include world/; headless SurvivalPressure owns the portable copy).
void applyVitals(Health& health, Survival& survival, float dt,
                 float oxygenDrainPerSecond, float hazardDamagePerSecond) {
    survival.oxygen = std::clamp(survival.oxygen - oxygenDrainPerSecond * dt, 0.0f, 100.0f);
    survival.hunger = std::clamp(survival.hunger - 0.12f * dt, 0.0f, 100.0f); // SurvivalRateTable::hungerDrainPerSecond
    float damage = hazardDamagePerSecond;
    if (survival.oxygen <= 0.0f) damage += 9.0f;   // asphyxiaDamagePerSecond
    if (survival.hunger <= 0.0f) damage += 1.5f;   // starvationDamagePerSecond
    if (damage > 0.0f) health.current = std::max(0.0f, health.current - damage * dt);
}

} // namespace

struct EcsWorld::Impl {
    entt::registry registry;
    entt::entity player{entt::null};
    std::uint64_t nextStableId{1};
    std::uint64_t nextCommandSequence{1};
    std::vector<PendingEcsCommand> commandBuffer;
};

const char* enemyArchetypeName(EnemyArchetype archetype) {
    switch(archetype) {
        case EnemyArchetype::Drone: return "Drone";
        case EnemyArchetype::Lictor: return "Lictor";
        case EnemyArchetype::Adept: return "Adept";
        case EnemyArchetype::Praetor: return "Praetor";
    }
    return "Enemy";
}

EcsWorld::EcsWorld() : impl_(std::make_unique<Impl>()) {}
EcsWorld::~EcsWorld() = default;
EcsWorld::EcsWorld(EcsWorld&&) noexcept = default;
EcsWorld& EcsWorld::operator=(EcsWorld&&) noexcept = default;

void EcsWorld::createPlayer(Vec3 position) {
    if (impl_->player != entt::null && impl_->registry.valid(impl_->player)) return;
    impl_->player = impl_->registry.create();
    impl_->registry.emplace<PlayerTag>(impl_->player);
    impl_->registry.emplace<Position>(impl_->player, position);
    impl_->registry.emplace<Velocity>(impl_->player, Vec3{});
    impl_->registry.emplace<Health>(impl_->player, 100.0f, 100.0f);
    impl_->registry.emplace<Survival>(impl_->player, 100.0f, 100.0f, 100.0f);
    impl_->registry.emplace<Grounded>(impl_->player, false);
    impl_->registry.emplace<Inventory>(impl_->player);
}

void EcsWorld::setPlayerPosition(Vec3 position) {
    auto& pos = impl_->registry.get<Position>(impl_->player).value;
    auto& vel = impl_->registry.get<Velocity>(impl_->player).value;
    pos = position;
    vel = {};
}

void EcsWorld::update(float dt,
                      const PlayerInput& input,
                      const SolidQuery& isSolid,
                      float oxygenDrainPerSecond,
                      float hazardDamagePerSecond) {
    if (impl_->player == entt::null) return;

    auto& pos = impl_->registry.get<Position>(impl_->player).value;
    auto& vel = impl_->registry.get<Velocity>(impl_->player).value;
    auto& grounded = impl_->registry.get<Grounded>(impl_->player).value;
    auto& health = impl_->registry.get<Health>(impl_->player);
    auto& survival = impl_->registry.get<Survival>(impl_->player);

    Vec3 forward = input.forward;
    forward.y = 0.0f;
    forward = normalize(forward);
    Vec3 right = normalize(cross(Vec3{0,1,0}, forward));
    Vec3 wish = forward * input.moveForward + right * input.moveRight;
    if (lengthSq(wish) > 1.0f) wish = normalize(wish);

    const float speed = input.sprint && survival.energy > 0.5f ? 6.7f : 4.5f;
    vel.x = wish.x * speed;
    vel.z = wish.z * speed;

    if (input.sprint && lengthSq(wish) > 0.01f) survival.energy = std::max(0.0f, survival.energy - 13.0f * dt); // energySprintDrainPerSecond
    else survival.energy = std::min(100.0f, survival.energy + 8.0f * dt); // energyRecoverPerSecond

    if (grounded && input.jump) {
        vel.y = 7.0f;
        grounded = false;
    }
    vel.y = std::max(-28.0f, vel.y - 18.0f * dt);

    Vec3 trial = pos;
    trial.x += vel.x * dt;
    if (!collides(trial, isSolid)) pos.x = trial.x; else vel.x = 0.0f;

    trial = pos;
    trial.z += vel.z * dt;
    if (!collides(trial, isSolid)) pos.z = trial.z; else vel.z = 0.0f;

    trial = pos;
    trial.y += vel.y * dt;
    if (!collides(trial, isSolid)) {
        pos.y = trial.y;
        grounded = false;
    } else {
        if (vel.y < 0.0f) grounded = true;
        vel.y = 0.0f;
    }

    applyVitals(health,survival,dt,oxygenDrainPerSecond,hazardDamagePerSecond);

    // AI: deterministic owner-thread structural mutation; destroy list is committed after iteration.
    const Vec3 playerCenter = pos + Vec3{0,0.9f,0};
    std::vector<entt::entity> dead;
    auto view = impl_->registry.view<EnemyTag, Position, Velocity, Health, DroneAI, CoordinateDomain, EnemyCombatProfile>();
    for (auto e : view) {
        if (view.get<CoordinateDomain>(e).value != ActorDomain::Planar) continue;
        auto& epos = view.get<Position>(e).value;
        auto& evel = view.get<Velocity>(e).value;
        auto& ehp = view.get<Health>(e);
        auto& ai = view.get<DroneAI>(e);
        const auto& combat = view.get<EnemyCombatProfile>(e);
        if (ehp.current <= 0.0f) { dead.push_back(e); continue; }

        Vec3 to = playerCenter - epos;
        const float dist = length(to);
        Vec3 dir = dist > 0.01f ? to / dist : Vec3{};
        evel = dir * combat.moveSpeed;
        if (dist > 1.4f) epos += evel * dt;

        ai.attackCooldown = std::max(0.0f, ai.attackCooldown - dt);
        if (dist < combat.attackRange && ai.attackCooldown <= 0.0f) {
            health.current = std::max(0.0f, health.current - combat.attackDamage);
            ai.attackCooldown = combat.attackPeriod;
        }
    }
    for (auto e : dead) impl_->registry.destroy(e);
}


void EcsWorld::updateVitals(float dt, float oxygenDrainPerSecond, float hazardDamagePerSecond, bool sprinting, bool moving) {
    if (impl_->player == entt::null) return;
    auto& health = impl_->registry.get<Health>(impl_->player);
    auto& survival = impl_->registry.get<Survival>(impl_->player);
    if (sprinting && moving && survival.energy > 0.5f) survival.energy = std::max(0.0f, survival.energy - 13.0f * dt); // energySprintDrainPerSecond
    else survival.energy = std::min(100.0f, survival.energy + 8.0f * dt); // energyRecoverPerSecond
    applyVitals(health,survival,dt,oxygenDrainPerSecond,hazardDamagePerSecond);
}

void EcsWorld::updateSurfaceEnemies(float dt, Vec3 playerCenter, const SurfaceHoverQuery& projectHover) {
    if (impl_->player == entt::null || !projectHover) return;
    auto& playerHealth = impl_->registry.get<Health>(impl_->player);
    std::vector<entt::entity> dead;
    auto view = impl_->registry.view<EnemyTag, Position, Velocity, Health, DroneAI, CoordinateDomain, EnemyCombatProfile>();
    for (auto e : view) {
        if (view.get<CoordinateDomain>(e).value != ActorDomain::PlanetSurface) continue;
        auto& epos = view.get<Position>(e).value;
        auto& evel = view.get<Velocity>(e).value;
        auto& ehp = view.get<Health>(e);
        auto& ai = view.get<DroneAI>(e);
        const auto& combat=view.get<EnemyCombatProfile>(e);
        if (ehp.current <= 0.0f) { dead.push_back(e); continue; }

        const Vec3 up = lengthSq(epos) > 0.001f ? normalize(epos) : Vec3{0,1,0};
        const Vec3 toPlayer = playerCenter - epos;
        const float radial = dot(toPlayer,up);
        const Vec3 tangent = toPlayer - up*radial;
        const float tangentDistance = length(tangent);
        const float spatialDistance = length(toPlayer);
        Vec3 dir{};
        if (tangentDistance > 0.01f) dir=tangent/tangentDistance;
        evel=dir*combat.moveSpeed;
        if (tangentDistance > 1.8f) {
            const Vec3 candidate=epos+evel*dt;
            epos=projectHover(candidate);
        } else {
            epos=projectHover(epos);
        }

        ai.attackCooldown=std::max(0.0f,ai.attackCooldown-dt);
        if (spatialDistance < combat.attackRange && ai.attackCooldown <= 0.0f) {
            playerHealth.current=std::max(0.0f,playerHealth.current-combat.attackDamage);
            ai.attackCooldown=combat.attackPeriod;
        }
    }
    for (auto e:dead) impl_->registry.destroy(e);
}


SurfaceAiTelemetry EcsWorld::updateSurfaceEnemiesPhased(JobSystem& jobs, float dt, Vec3 playerCenter,
                                                         const SurfaceHoverQuery& projectHover,
                                                         const SurfacePathQuery& resolvePath,
                                                         const PlayerDamageFilter& mitigatePlayerDamage) {
    SurfaceAiTelemetry telemetry{};
    if (impl_->player == entt::null || !projectHover) return telemetry;

    struct ReadState {
        entt::entity entity{entt::null};
        std::uint64_t stableId{};
        Vec3 position{};
        float health{};
        float maxHealth{};
        float cooldown{};
        EnemyArchetype archetype{EnemyArchetype::Drone};
        EnemyCombatProfile combat{};
    };
    struct SenseState {
        Vec3 tangentDirection{};
        float tangentDistance{};
        float spatialDistance{};
    };
    struct Intent {
        Vec3 velocity{};
        bool move{};
        bool attack{};
        float nextCooldown{};
        std::uint64_t supportTargetStableId{};
        float supportHeal{};
    };
    struct PathState {
        Vec3 position{};
        bool requested{};
        bool resolved{};
    };

    // Owner-thread snapshot: workers never read or structurally mutate EnTT.
    std::vector<ReadState> reads;
    auto view=impl_->registry.view<EnemyTag,StableId,Position,Health,DroneAI,CoordinateDomain,EnemyKind,EnemyCombatProfile>();
    for(auto e:view) {
        if(view.get<CoordinateDomain>(e).value!=ActorDomain::PlanetSurface) continue;
        const auto& hp=view.get<Health>(e);
        reads.push_back({e,view.get<StableId>(e).value,view.get<Position>(e).value,
                         hp.current,hp.max,view.get<DroneAI>(e).attackCooldown,
                         view.get<EnemyKind>(e).value,view.get<EnemyCombatProfile>(e)});
    }
    std::sort(reads.begin(),reads.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    if(reads.empty()) return telemetry;

    std::vector<SenseState> senses(reads.size());
    std::vector<Intent> intents(reads.size());
    std::vector<PathState> paths(reads.size());

    // Sense phase: immutable geometric reads only.
    jobs.parallelFor(reads.size(),[&](std::size_t i) {
        const auto& r=reads[i];
        const Vec3 up=lengthSq(r.position)>0.001f?normalize(r.position):Vec3{0,1,0};
        const Vec3 toPlayer=playerCenter-r.position;
        const float radial=dot(toPlayer,up);
        const Vec3 tangent=toPlayer-up*radial;
        const float tangentDistance=length(tangent);
        SenseState sense{};
        sense.tangentDistance=tangentDistance;
        sense.spatialDistance=length(toPlayer);
        if(tangentDistance>0.01f) sense.tangentDirection=tangent/tangentDistance;
        senses[i]=sense;
    },8);
    telemetry.sensed=static_cast<int>(reads.size());

    // Think phase: pure intent selection. No registry/world mutation occurs.
    jobs.parallelFor(reads.size(),[&](std::size_t i) {
        const auto& read=reads[i];
        const auto& sense=senses[i];
        Intent intent{};
        intent.nextCooldown=std::max(0.0f,read.cooldown-dt);
        if(read.health>0.0f) {
            intent.velocity=sense.tangentDirection*read.combat.moveSpeed;
            intent.move=sense.tangentDistance>1.8f;
            intent.attack=sense.spatialDistance<read.combat.attackRange && intent.nextCooldown<=0.0f;
            if(intent.attack) intent.nextCooldown=read.combat.attackPeriod;
            if(read.archetype==EnemyArchetype::Adept && read.combat.supportHeal>0.0f && read.cooldown<=0.0f) {
                float bestFraction=1.0f;
                std::uint64_t bestId=0;
                for(const auto& ally:reads) {
                    if(ally.stableId==read.stableId || ally.health<=0.0f || ally.health>=ally.maxHealth) continue;
                    if(lengthSq(ally.position-read.position)>read.combat.supportRange*read.combat.supportRange) continue;
                    const float fraction=ally.maxHealth>0.0f?ally.health/ally.maxHealth:1.0f;
                    if(fraction<bestFraction || (std::abs(fraction-bestFraction)<1e-6f && ally.stableId<bestId)) {
                        bestFraction=fraction; bestId=ally.stableId;
                    }
                }
                if(bestId!=0) {
                    intent.supportTargetStableId=bestId;
                    intent.supportHeal=read.combat.supportHeal;
                    intent.nextCooldown=std::max(intent.nextCooldown,2.2f);
                }
            }
        }
        intents[i]=intent;
    },8);
    telemetry.thought=static_cast<int>(reads.size());

    // Path Request phase: workers resolve a bounded local steering/path query
    // against read-only world state. This is intentionally not a global navmesh
    // yet, but it is a distinct phase and does not mutate EnTT or PlanetSurface.
    jobs.parallelFor(reads.size(),[&](std::size_t i) {
        PathState path{};
        path.position=reads[i].position;
        if(intents[i].move) {
            path.requested=true;
            if(resolvePath) {
                path.position=resolvePath(reads[i].position,intents[i].velocity,dt);
                path.resolved=true;
            } else {
                path.position=projectHover(reads[i].position+intents[i].velocity*dt);
                path.resolved=true;
            }
        } else {
            path.position=projectHover(reads[i].position);
            path.resolved=true;
        }
        paths[i]=path;
    },8);
    for(const auto& path:paths) {
        if(path.requested) ++telemetry.pathRequested;
        if(path.resolved) ++telemetry.pathResolved;
    }

    // Commit phase: deterministic stable-ID order on the owner thread. World
    // projection, ECS mutation, player damage, and destruction stay here.
    auto& playerHealth=impl_->registry.get<Health>(impl_->player);
    std::vector<entt::entity> dead;
    dead.reserve(reads.size());
    for(std::size_t i=0;i<reads.size();++i) {
        const auto& read=reads[i];
        if(!impl_->registry.valid(read.entity)) continue;
        auto& hp=impl_->registry.get<Health>(read.entity);
        if(hp.current<=0.0f) { dead.push_back(read.entity); continue; }
        const auto& intent=intents[i];
        auto& pos=impl_->registry.get<Position>(read.entity).value;
        auto& vel=impl_->registry.get<Velocity>(read.entity).value;
        auto& ai=impl_->registry.get<DroneAI>(read.entity);
        vel=intent.velocity;
        pos=paths[i].position;
        ai.attackCooldown=intent.nextCooldown;
        if(intent.supportTargetStableId!=0 && intent.supportHeal>0.0f) {
            auto targetView=impl_->registry.view<EnemyTag,StableId,Health>();
            for(auto target:targetView) {
                if(targetView.get<StableId>(target).value!=intent.supportTargetStableId) continue;
                auto& targetHp=targetView.get<Health>(target);
                const float before=targetHp.current;
                targetHp.current=std::min(targetHp.max,targetHp.current+intent.supportHeal);
                const float healed=std::max(0.0f,targetHp.current-before);
                if(healed>0.0f) { ++telemetry.supportActions; telemetry.supportHealing+=healed; }
                break;
            }
        }
        if(intent.attack) {
            const float rawDamage=read.combat.attackDamage;
            float applied=rawDamage;
            if(mitigatePlayerDamage) applied=std::clamp(mitigatePlayerDamage(read.position,rawDamage),0.0f,rawDamage);
            playerHealth.current=std::max(0.0f,playerHealth.current-applied);
            telemetry.attackDamageAttempted+=rawDamage;
            telemetry.attackDamageApplied+=applied;
            ++telemetry.attacks;
        }
        ++telemetry.committed;
    }
    for(auto e:dead) if(impl_->registry.valid(e))
        queueDestroyEnemy(impl_->registry.get<StableId>(e).value);
    flushCommands();
    return telemetry;
}

PlayerSnapshot EcsWorld::player() const {
    PlayerSnapshot out{};
    if (impl_->player == entt::null) return out;
    out.position = impl_->registry.get<Position>(impl_->player).value;
    out.velocity = impl_->registry.get<Velocity>(impl_->player).value;
    const auto& hp = impl_->registry.get<Health>(impl_->player);
    const auto& s = impl_->registry.get<Survival>(impl_->player);
    out.health = hp.current;
    out.oxygen = s.oxygen;
    out.energy = s.energy;
    out.hunger = s.hunger;
    out.grounded = impl_->registry.get<Grounded>(impl_->player).value;
    return out;
}

PlayerPersistentState EcsWorld::savePlayer() const {
    PlayerPersistentState out{};
    if (impl_->player == entt::null) return out;
    const auto snap = player();
    out.position = snap.position;
    out.health = snap.health;
    out.oxygen = snap.oxygen;
    out.energy = snap.energy;
    out.hunger = snap.hunger;
    const auto& inv = impl_->registry.get<Inventory>(impl_->player).items;
    out.inventory.reserve(inv.size());
    for (const auto& [id, count] : inv) if (count > 0) out.inventory.emplace_back(id,count);
    std::sort(out.inventory.begin(), out.inventory.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    return out;
}

void EcsWorld::loadPlayer(const PlayerPersistentState& state) {
    if (impl_->player == entt::null) createPlayer(state.position);
    impl_->registry.get<Position>(impl_->player).value = state.position;
    impl_->registry.get<Velocity>(impl_->player).value = {};
    auto& hp = impl_->registry.get<Health>(impl_->player);
    hp.current = std::clamp(state.health, 0.0f, hp.max);
    auto& s = impl_->registry.get<Survival>(impl_->player);
    s.oxygen = std::clamp(state.oxygen, 0.0f, 100.0f);
    s.energy = std::clamp(state.energy, 0.0f, 100.0f);
    s.hunger = std::clamp(state.hunger, 0.0f, 100.0f);
    auto& inv = impl_->registry.get<Inventory>(impl_->player).items;
    inv.clear();
    for (const auto& [id,count] : state.inventory) if (count > 0) inv[id] = count;
}

void EcsWorld::spawnEnemy(Vec3 position, EnemyArchetype archetype, std::uint64_t stableId, ActorDomain domain) {
    const auto e = impl_->registry.create();
    if (stableId == 0) stableId = impl_->nextStableId++;
    else impl_->nextStableId = std::max(impl_->nextStableId, stableId + 1);
    const auto profile=profileFor(archetype);
    impl_->registry.emplace<EnemyTag>(e);
    impl_->registry.emplace<StableId>(e, stableId);
    impl_->registry.emplace<Position>(e, position);
    impl_->registry.emplace<Velocity>(e, Vec3{});
    impl_->registry.emplace<Health>(e, profile.health, profile.health);
    impl_->registry.emplace<DroneAI>(e, 0.3f);
    impl_->registry.emplace<CoordinateDomain>(e, domain);
    impl_->registry.emplace<EnemyKind>(e, archetype);
    impl_->registry.emplace<EnemyCombatProfile>(e, profile.combat);
}

void EcsWorld::spawnDrone(Vec3 position, std::uint64_t stableId, ActorDomain domain) {
    spawnEnemy(position,EnemyArchetype::Drone,stableId,domain);
}

void EcsWorld::queueSpawnEnemy(Vec3 position,EnemyArchetype archetype,std::uint64_t stableId,ActorDomain domain) {
    PendingEcsCommand cmd{};
    cmd.type=EcsCommandType::SpawnDrone;
    cmd.sequence=impl_->nextCommandSequence++;
    cmd.position=position;
    cmd.stableId=stableId;
    cmd.domain=domain;
    cmd.archetype=archetype;
    impl_->commandBuffer.push_back(cmd);
}

void EcsWorld::queueSpawnDrone(Vec3 position,std::uint64_t stableId,ActorDomain domain) {
    queueSpawnEnemy(position,EnemyArchetype::Drone,stableId,domain);
}

void EcsWorld::queueDestroyEnemy(std::uint64_t stableId) {
    if(stableId==0) return;
    PendingEcsCommand cmd{}; cmd.type=EcsCommandType::DestroyEnemy; cmd.sequence=impl_->nextCommandSequence++; cmd.stableId=stableId; impl_->commandBuffer.push_back(cmd);
}

void EcsWorld::queueDamageEnemy(std::uint64_t stableId,float damage) {
    if(stableId==0 || damage<=0.0f) return;
    PendingEcsCommand cmd{}; cmd.type=EcsCommandType::DamageEnemy; cmd.sequence=impl_->nextCommandSequence++; cmd.stableId=stableId; cmd.amount=damage; impl_->commandBuffer.push_back(cmd);
}

int EcsWorld::queuedCommandCount() const {
    return static_cast<int>(impl_->commandBuffer.size());
}

EcsCommandTelemetry EcsWorld::flushCommands() {
    EcsCommandTelemetry out{};
    out.queued=static_cast<int>(impl_->commandBuffer.size());
    std::stable_sort(impl_->commandBuffer.begin(),impl_->commandBuffer.end(),[](const auto& a,const auto& b){
        return a.sequence<b.sequence;
    });
    for(const auto& cmd:impl_->commandBuffer) {
        if(cmd.type==EcsCommandType::SpawnDrone) {
            spawnEnemy(cmd.position,   cmd.positi,cmd.stableId,cmd.domain);
            ++out.spawned;
            continue;
        }
        entt::entity victim=entt::null;
        auto view=impl_->registry.view<EnemyTag,StableId>();
        for(auto e:view) if(view.get<StableId>(e).value==cmd.stableId) { victim=e; break; }
        if(victim==entt::null || !impl_->registry.valid(victim)) continue;
        if(cmd.type==EcsCommandType::DamageEnemy) {
            auto& hp=impl_->registry.get<Health>(victim);
            hp.current-=std::max(0.0f,cmd.amount);
            ++out.damaged;
            if(hp.current<=0.0f) { impl_->registry.destroy(victim); ++out.destroyed; }
        } else {
            impl_->registry.destroy(victim);
            ++out.destroyed;
        }
    }
    impl_->commandBuffer.clear();
    return out;
}

void EcsWorld::clearEnemies() {
    impl_->commandBuffer.clear();
    auto view = impl_->registry.view<EnemyTag>();
    std::vector<entt::entity> doomed;
    for (auto e : view) doomed.push_back(e);
    for (auto e : doomed) impl_->registry.destroy(e);
}

std::vector<EnemySnapshot> EcsWorld::enemies() const {
    std::vector<EnemySnapshot> out;
    auto view = impl_->registry.view<EnemyTag, StableId, Position, Health, CoordinateDomain, EnemyKind>();
    for (auto e : view) {
        out.push_back({view.get<StableId>(e).value, view.get<Position>(e).value, view.get<Health>(e).current, view.get<CoordinateDomain>(e).value, view.get<EnemyKind>(e).value});
    }
    return out;
}

std::vector<EnemySnapshot> EcsWorld::enemies(ActorDomain domain) const {
    std::vector<EnemySnapshot> out;
    auto view = impl_->registry.view<EnemyTag, StableId, Position, Health, CoordinateDomain, EnemyKind>();
    for (auto e:view) {
        if (view.get<CoordinateDomain>(e).value != domain) continue;
        out.push_back({view.get<StableId>(e).value, view.get<Position>(e).value, view.get<Health>(e).current, domain, view.get<EnemyKind>(e).value});
    }
    return out;
}

RayAttackResult EcsWorld::attackRay(Vec3 origin, Vec3 direction, float maxDistance, float damage, ActorDomain domain) {
    direction = normalize(direction);
    RayAttackResult result{};
    float best = maxDistance + 1.0f;
    entt::entity bestEntity = entt::null;
    auto view = impl_->registry.view<EnemyTag, StableId, Position, Health, CoordinateDomain>();
    for (auto e : view) {
        if (view.get<CoordinateDomain>(e).value != domain) continue;
        const Vec3 p=view.get<Position>(e).value;
        const Vec3 up = domain==ActorDomain::PlanetSurface && lengthSq(p)>0.001f ? normalize(p) : Vec3{0,1,0};
        const Vec3 center = p + up*0.45f;
        const float t = raySphere(origin, direction, center, 0.48f);
        if (t <= maxDistance && t < best) { best = t; bestEntity = e; }
    }
    if (bestEntity != entt::null) {
        auto& hp = impl_->registry.get<Health>(bestEntity);
        hp.current -= damage;
        result.hit = true;
        result.distance = best;
        result.stableId = impl_->registry.get<StableId>(bestEntity).value;
        if (hp.current <= 0.0f) {
            result.killed = true;
            impl_->registry.destroy(bestEntity);
        }
    }
    return result;
}

int EcsWorld::inventoryCount(int itemId) const {
    if (impl_->player == entt::null) return 0;
    const auto& inv = impl_->registry.get<Inventory>(impl_->player).items;
    const auto it = inv.find(itemId);
    return it == inv.end() ? 0 : it->second;
}

void EcsWorld::addItem(int itemId, int count) {
    if (impl_->player == entt::null || count <= 0) return;
    impl_->registry.get<Inventory>(impl_->player).items[itemId] += count;
}

bool EcsWorld::consumeItem(int itemId, int count) {
    if (impl_->player == entt::null || count <= 0) return false;
    auto& inv = impl_->registry.get<Inventory>(impl_->player).items;
    auto it = inv.find(itemId);
    if (it == inv.end() || it->second < count) return false;
    it->second -= count;
    if (it->second <= 0) inv.erase(it);
    return true;
}

} // namespace elysium
