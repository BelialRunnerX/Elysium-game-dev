#include "world/SurfaceInfrastructure.hpp"

#include "core/Determinism.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace elysium {
namespace {

constexpr float kLinkRadius = 12.0f;
constexpr float kBurnerGeneration = 20.0f;
constexpr float kAtmosphereDemand = 8.0f;
constexpr float kAirlockControllerDemand = 2.5f;
constexpr float kBatteryCapacity = 60.0f;
constexpr float kBatteryChargeRate = 15.0f;
constexpr float kBatteryDischargeRate = 15.0f;
constexpr float kPressurizeRate = 0.22f;
constexpr float kOxygenateRate = 0.28f;
constexpr float kVentPressureRate = 0.65f;
constexpr float kVentOxygenRate = 0.90f;
constexpr float kAirlockDepressurizeRate = 0.75f;
constexpr float kAirlockDeoxygenateRate = 0.95f;
constexpr float kAirlockPressurizeRate = 0.60f;
constexpr float kAirlockOxygenateRate = 0.70f;
constexpr float kSensorDemand = 1.5f;
constexpr float kTurretDemand = 4.0f;
constexpr float kShieldDemand = 12.0f;
constexpr float kLogicDemand = 1.0f;
constexpr float kAlloyCrucibleDemand = 5.0f;
constexpr float kRefineryDemand = 8.0f;
constexpr float kNetworkStorageDemand = 1.5f;
constexpr float kConveyorDemand = 0.75f;
constexpr float kSorterDemand = 1.0f;
constexpr float kCargoLoaderDemand = 2.0f;
constexpr float kCrusherDemand = 4.0f;
constexpr float kChemicalVatDemand = 6.0f;
constexpr float kFabricatorDemand = 9.0f;
constexpr float kExtractorDemand = 10.0f;
constexpr float kSensorRange = 30.0f;
constexpr float kTurretRange = 20.0f;
constexpr float kTurretDamage = 9.0f;
constexpr float kTurretCooldown = 0.45f;
constexpr float kShieldRadius = 10.0f;
constexpr float kShieldCapacity = 120.0f;
constexpr float kShieldRechargeRate = 18.0f;

float demandFor(MachineType type) {
    switch(type) {
        case MachineType::AtmosphereUnit: return kAtmosphereDemand;
        case MachineType::AirlockController: return kAirlockControllerDemand;
        case MachineType::SensorMast: return kSensorDemand;
        case MachineType::Turret: return kTurretDemand;
        case MachineType::ShieldPylon: return kShieldDemand;
        case MachineType::LogicController: return kLogicDemand;
        case MachineType::AlloyCrucible: return kAlloyCrucibleDemand;
        case MachineType::Refinery: return kRefineryDemand;
        case MachineType::NetworkStorage: return kNetworkStorageDemand;
        case MachineType::Conveyor: return kConveyorDemand;
        case MachineType::Sorter: return kSorterDemand;
        case MachineType::CargoLoader: return kCargoLoaderDemand;
        case MachineType::Crusher: return kCrusherDemand;
        case MachineType::ChemicalVat: return kChemicalVatDemand;
        case MachineType::Fabricator: return kFabricatorDemand;
        case MachineType::Extractor: return kExtractorDemand;
        default: return 0.0f;
    }
}
int priorityFor(MachineType type) {
    switch(type) {
        case MachineType::AtmosphereUnit: return 1;
        case MachineType::AirlockController: return 2;
        case MachineType::SensorMast:
        case MachineType::Turret:
        case MachineType::ShieldPylon:
        case MachineType::LogicController: return 2;
        case MachineType::NetworkStorage:
        case MachineType::Conveyor:
        case MachineType::Sorter:
        case MachineType::CargoLoader: return 3;
        case MachineType::AlloyCrucible:
        case MachineType::Refinery:
        case MachineType::Crusher:
        case MachineType::ChemicalVat:
        case MachineType::Fabricator:
        case MachineType::Extractor: return 4;
        default: return 5;
    }
}

struct Dsu {
    std::vector<int> p;
    explicit Dsu(int n):p(static_cast<std::size_t>(n)){std::iota(p.begin(),p.end(),0);}
    int find(int x){return p[static_cast<std::size_t>(x)]==x?x:(p[static_cast<std::size_t>(x)]=find(p[static_cast<std::size_t>(x)]));}
    void join(int a,int b){a=find(a);b=find(b);if(a!=b)p[static_cast<std::size_t>(b)]=a;}
};

} // namespace

const char* surfacePortalName(SurfacePortalType type) {
    switch(type) {
        case SurfacePortalType::Door: return "Door";
        case SurfacePortalType::Airlock: return "Airlock";
    }
    return "Portal";
}

BlockType surfacePortalBlockType(SurfacePortalType type) {
    return type==SurfacePortalType::Airlock ? BlockType::AirlockPanel : BlockType::DoorPanel;
}

const char* surfaceAirlockStateName(SurfaceAirlockState state) {
    switch(state) {
        case SurfaceAirlockState::Idle: return "IDLE";
        case SurfaceAirlockState::Depressurizing: return "DEPRESSURIZING";
        case SurfaceAirlockState::ExteriorOpen: return "EXTERIOR OPEN";
        case SurfaceAirlockState::Pressurizing: return "PRESSURIZING";
        case SurfaceAirlockState::InteriorOpen: return "INTERIOR OPEN";
        case SurfaceAirlockState::Fault: return "FAULT";
    }
    return "UNKNOWN";
}

const char* surfaceAutomationTriggerName(SurfaceAutomationTrigger trigger) {
    switch(trigger) {
        case SurfaceAutomationTrigger::HostilesDetected: return "HOSTILES DETECTED";
        case SurfaceAutomationTrigger::BatteryBelow: return "BATTERY BELOW";
        case SurfaceAutomationTrigger::AtmospherePressureBelow: return "PRESSURE BELOW";
    }
    return "UNKNOWN";
}

const char* surfaceAutomationActionName(SurfaceAutomationAction action) {
    switch(action) {
        case SurfaceAutomationAction::EnableMachine: return "ENABLE MACHINE";
        case SurfaceAutomationAction::DisableMachine: return "DISABLE MACHINE";
        case SurfaceAutomationAction::ClosePortal: return "CLOSE PORTAL";
    }
    return "UNKNOWN";
}

SurfaceInfrastructure::SurfaceInfrastructure(std::uint64_t worldSeed):worldSeed_(worldSeed){}

std::uint64_t SurfaceInfrastructure::allocateStableId() {
    for (;;) {
        const std::uint64_t id=mix64(worldSeed_^0x535552464F424AULL^nextSerial_++);
        if (id!=0 && !find(id) && !findPortal(id) && !findAirlockAssembly(id) && !findAutomationRule(id)) return id;
    }
}

std::uint64_t SurfaceInfrastructure::place(MachineType type, SurfaceCellAddress anchor) {
    SurfaceMachineObject o{};
    o.stableId=allocateStableId();
    o.type=type;
    o.anchor=anchor;
    if(type==MachineType::Turret) o.ammo=48;
    if(type==MachineType::ShieldPylon) o.shieldCharge=0.0f;
    objects_.push_back(o);
    return o.stableId;
}

bool SurfaceInfrastructure::restore(const SurfaceMachineObject& object) {
    if (object.stableId==0 || find(object.stableId) || findPortal(object.stableId) || findAirlockAssembly(object.stableId)) return false;
    SurfaceMachineObject copy=object;
    copy.powered=false;
    copy.fuelSeconds=std::max(0.0f,copy.fuelSeconds);
    if(copy.type==MachineType::BatteryBank) copy.storedEnergy=std::clamp(copy.storedEnergy,0.0f,kBatteryCapacity);
    copy.roomPressure=std::clamp(copy.roomPressure,0.0f,1.0f);
    copy.roomOxygen=std::clamp(copy.roomOxygen,0.0f,1.0f);
    copy.roomSealed=false;
    copy.ammo=std::max(0,copy.ammo);
    copy.shieldCharge=std::clamp(copy.shieldCharge,0.0f,kShieldCapacity);
    copy.cooldownSeconds=0.0f;
    copy.powerNetworkId=0;
    copy.detectedHostiles=0;
    copy.processProgressSeconds=std::max(0.0f,copy.processProgressSeconds);
    copy.extractorProgressSeconds=std::max(0.0f,copy.extractorProgressSeconds);
    copy.logisticsProgressSeconds=std::max(0.0f,copy.logisticsProgressSeconds);
    copy.logisticsBlocked=false;
    // Normalize persistent inventory so malformed duplicate/zero stacks cannot
    // create nondeterministic process results after restore.
    std::sort(copy.inventory.begin(),copy.inventory.end(),[](const auto& a,const auto& b){return a.itemId<b.itemId;});
    std::vector<SurfaceItemStack> normalized;
    for(const auto& stack:copy.inventory) {
        if(stack.itemId<=0 || stack.count<=0) continue;
        if(!normalized.empty() && normalized.back().itemId==stack.itemId) normalized.back().count+=stack.count;
        else normalized.push_back(stack);
    }
    copy.inventory=std::move(normalized);
    objects_.push_back(copy);
    return true;
}

bool SurfaceInfrastructure::remove(std::uint64_t stableId) {
    if(airlockForController(stableId)) return false;
    if(std::any_of(automationRules_.begin(),automationRules_.end(),[&](const auto& r){return r.controllerMachineId==stableId || r.sourceStableId==stableId || r.targetStableId==stableId;})) return false;
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    if(it==objects_.end()) return false;
    objects_.erase(it);
    return true;
}

SurfaceMachineObject* SurfaceInfrastructure::find(std::uint64_t stableId) {
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    return it==objects_.end()?nullptr:&*it;
}
const SurfaceMachineObject* SurfaceInfrastructure::find(std::uint64_t stableId) const {
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    return it==objects_.end()?nullptr:&*it;
}

bool SurfaceInfrastructure::occupied(SurfaceCellAddress anchor) const {
    return std::any_of(objects_.begin(),objects_.end(),[&](const auto& o){return o.anchor==anchor;});
}

SurfaceMachineObject* SurfaceInfrastructure::nearest(const PlanetSurface& planet, MachineType type, Vec3 position, float maxDistance) {
    SurfaceMachineObject* best=nullptr;
    float bestSq=maxDistance*maxDistance;
    for(auto& o:objects_) {
        if(o.type!=type) continue;
        const Vec3 c=planet.cellCenterPosition(o.anchor);
        const float d=lengthSq(c-position);
        if(d<=bestSq){bestSq=d;best=&o;}
    }
    return best;
}

std::uint64_t SurfaceInfrastructure::placePortal(PlanetSurface& planet, SurfacePortalType type, SurfaceCellAddress anchor, bool open) {
    anchor=planet.normalize(anchor);
    if(!planet.radialInBounds(anchor.radial) || occupied(anchor) || portalOccupied(anchor)) return 0;
    const std::uint64_t id=allocateStableId();
    SurfacePortalObject portal{id,type,anchor,open};
    portals_.push_back(portal);
    planet.set(anchor,open?BlockType::Air:surfacePortalBlockType(type),true);
    return id;
}

bool SurfaceInfrastructure::restorePortal(const SurfacePortalObject& object) {
    if(object.stableId==0 || find(object.stableId) || findPortal(object.stableId) || findAirlockAssembly(object.stableId) || portalOccupied(object.anchor)) return false;
    portals_.push_back(object);
    return true;
}

bool SurfaceInfrastructure::removePortal(PlanetSurface& planet,std::uint64_t stableId,bool clearCell) {
    if(airlockForPortal(stableId)) return false; // break the assembly explicitly first.
    const auto it=std::find_if(portals_.begin(),portals_.end(),[&](const auto& p){return p.stableId==stableId;});
    if(it==portals_.end()) return false;
    if(clearCell) planet.set(it->anchor,BlockType::Air,true);
    portals_.erase(it);
    return true;
}

bool SurfaceInfrastructure::setPortalOpenUnchecked(PlanetSurface& planet,std::uint64_t stableId,bool open) {
    auto* portal=findPortal(stableId);
    if(!portal) return false;
    if(portal->open==open) return true;
    portal->open=open;
    planet.set(portal->anchor,open?BlockType::Air:surfacePortalBlockType(portal->type),true);
    return true;
}

bool SurfaceInfrastructure::setPortalOpen(PlanetSurface& planet,std::uint64_t stableId,bool open) {
    if(open && airlockForPortal(stableId)) return false; // powered assembly owns opening authority.
    return setPortalOpenUnchecked(planet,stableId,open);
}

SurfacePortalObject* SurfaceInfrastructure::findPortal(std::uint64_t stableId) {
    const auto it=std::find_if(portals_.begin(),portals_.end(),[&](const auto& p){return p.stableId==stableId;});
    return it==portals_.end()?nullptr:&*it;
}

const SurfacePortalObject* SurfaceInfrastructure::findPortal(std::uint64_t stableId) const {
    const auto it=std::find_if(portals_.begin(),portals_.end(),[&](const auto& p){return p.stableId==stableId;});
    return it==portals_.end()?nullptr:&*it;
}

bool SurfaceInfrastructure::portalOccupied(SurfaceCellAddress anchor) const {
    return std::any_of(portals_.begin(),portals_.end(),[&](const auto& p){return p.anchor==anchor;});
}

SurfacePortalObject* SurfaceInfrastructure::nearestPortal(const PlanetSurface& planet,Vec3 position,float maxDistance) {
    SurfacePortalObject* best=nullptr;
    float bestSq=maxDistance*maxDistance;
    for(auto& portal:portals_) {
        const float d=lengthSq(planet.cellCenterPosition(portal.anchor)-position);
        if(d<=bestSq){bestSq=d;best=&portal;}
    }
    return best;
}

std::uint64_t SurfaceInfrastructure::createAirlockAssembly(PlanetSurface& planet,
                                                           std::uint64_t controllerMachineId,
                                                           std::uint64_t innerPortalId,
                                                           std::uint64_t outerPortalId,
                                                           SurfaceCellAddress chamberAnchor,
                                                           float initialPressure,
                                                           float initialOxygen) {
    auto* controller=find(controllerMachineId);
    auto* inner=findPortal(innerPortalId);
    auto* outer=findPortal(outerPortalId);
    if(!controller || controller->type!=MachineType::AirlockController || !inner || !outer || inner==outer) return 0;
    if(inner->type!=SurfacePortalType::Airlock || outer->type!=SurfacePortalType::Airlock) return 0;
    if(airlockForController(controllerMachineId) || airlockForPortal(innerPortalId) || airlockForPortal(outerPortalId)) return 0;
    chamberAnchor=planet.normalize(chamberAnchor);
    if(!planet.radialInBounds(chamberAnchor.radial)) return 0;

    const std::uint64_t stableId=allocateStableId();
    SurfaceAirlockAssembly assembly{};
    assembly.stableId=stableId;
    assembly.controllerMachineId=controllerMachineId;
    assembly.innerPortalId=innerPortalId;
    assembly.outerPortalId=outerPortalId;
    assembly.chamberAnchor=chamberAnchor;
    assembly.state=SurfaceAirlockState::Idle;
    assembly.chamberPressure=std::clamp(initialPressure,0.0f,1.0f);
    assembly.chamberOxygen=std::clamp(initialOxygen,0.0f,assembly.chamberPressure);
    airlocks_.push_back(assembly);
    setPortalOpenUnchecked(planet,innerPortalId,false);
    setPortalOpenUnchecked(planet,outerPortalId,false);
    return stableId;
}

bool SurfaceInfrastructure::restoreAirlockAssembly(const SurfaceAirlockAssembly& assembly) {
    if(assembly.stableId==0 || find(assembly.stableId) || findPortal(assembly.stableId) || findAirlockAssembly(assembly.stableId)) return false;
    if(assembly.innerPortalId==0 || assembly.outerPortalId==0 || assembly.innerPortalId==assembly.outerPortalId || assembly.controllerMachineId==0) return false;
    if(airlockForController(assembly.controllerMachineId) || airlockForPortal(assembly.innerPortalId) || airlockForPortal(assembly.outerPortalId)) return false;
    SurfaceAirlockAssembly copy=assembly;
    copy.chamberPressure=std::clamp(copy.chamberPressure,0.0f,1.0f);
    copy.chamberOxygen=std::clamp(copy.chamberOxygen,0.0f,copy.chamberPressure);
    airlocks_.push_back(copy);
    return true;
}

bool SurfaceInfrastructure::removeAirlockAssembly(std::uint64_t stableId) {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.stableId==stableId;});
    if(it==airlocks_.end()) return false;
    airlocks_.erase(it);
    return true;
}

SurfaceAirlockAssembly* SurfaceInfrastructure::findAirlockAssembly(std::uint64_t stableId) {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.stableId==stableId;});
    return it==airlocks_.end()?nullptr:&*it;
}

const SurfaceAirlockAssembly* SurfaceInfrastructure::findAirlockAssembly(std::uint64_t stableId) const {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.stableId==stableId;});
    return it==airlocks_.end()?nullptr:&*it;
}

SurfaceAirlockAssembly* SurfaceInfrastructure::airlockForController(std::uint64_t controllerMachineId) {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.controllerMachineId==controllerMachineId;});
    return it==airlocks_.end()?nullptr:&*it;
}

const SurfaceAirlockAssembly* SurfaceInfrastructure::airlockForController(std::uint64_t controllerMachineId) const {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.controllerMachineId==controllerMachineId;});
    return it==airlocks_.end()?nullptr:&*it;
}

SurfaceAirlockAssembly* SurfaceInfrastructure::airlockForPortal(std::uint64_t portalId) {
    const auto it=std::find_if(airlocks_.begin(),airlocks_.end(),[&](const auto& a){return a.innerPortalId==portalId || a.outerPortalId==portalId;});
    return it==airlocks_.end()?nullptr:&*it;
}

bool SurfaceInfrastructure::requestAirlockCycle(PlanetSurface& planet,std::uint64_t stableId,SurfaceAirlockDestination destination) {
    auto* assembly=findAirlockAssembly(stableId);
    if(!assembly) return false;
    auto* controller=find(assembly->controllerMachineId);
    auto* inner=findPortal(assembly->innerPortalId);
    auto* outer=findPortal(assembly->outerPortalId);
    if(!controller || controller->type!=MachineType::AirlockController || !inner || !outer ||
       inner->type!=SurfacePortalType::Airlock || outer->type!=SurfacePortalType::Airlock) {
        assembly->state=SurfaceAirlockState::Fault;
        return false;
    }
    setPortalOpenUnchecked(planet,assembly->innerPortalId,false);
    setPortalOpenUnchecked(planet,assembly->outerPortalId,false);
    assembly->state=destination==SurfaceAirlockDestination::Exterior
        ? SurfaceAirlockState::Depressurizing
        : SurfaceAirlockState::Pressurizing;
    return true;
}


std::uint64_t SurfaceInfrastructure::createAutomationRule(std::uint64_t controllerMachineId,
                                                          SurfaceAutomationTrigger trigger,
                                                          std::uint64_t sourceStableId,
                                                          float threshold,
                                                          SurfaceAutomationAction action,
                                                          std::uint64_t targetStableId) {
    const auto* controller=find(controllerMachineId);
    if(!controller || controller->type!=MachineType::LogicController || !find(sourceStableId)) return 0;
    if(action==SurfaceAutomationAction::ClosePortal) { if(!findPortal(targetStableId)) return 0; }
    else if(!find(targetStableId)) return 0;
    SurfaceAutomationRule rule{};
    rule.stableId=allocateStableId();
    rule.controllerMachineId=controllerMachineId;
    rule.trigger=trigger;
    rule.sourceStableId=sourceStableId;
    rule.threshold=std::max(0.0f,threshold);
    rule.action=action;
    rule.targetStableId=targetStableId;
    automationRules_.push_back(rule);
    return rule.stableId;
}

bool SurfaceInfrastructure::restoreAutomationRule(const SurfaceAutomationRule& rule) {
    if(rule.stableId==0 || find(rule.stableId) || findPortal(rule.stableId) || findAirlockAssembly(rule.stableId) || findAutomationRule(rule.stableId)) return false;
    const auto* controller=find(rule.controllerMachineId);
    if(!controller || controller->type!=MachineType::LogicController) return false;
    SurfaceAutomationRule copy=rule;
    copy.threshold=std::max(0.0f,copy.threshold);
    automationRules_.push_back(copy);
    return true;
}

bool SurfaceInfrastructure::removeAutomationRule(std::uint64_t stableId) {
    const auto it=std::find_if(automationRules_.begin(),automationRules_.end(),[&](const auto& r){return r.stableId==stableId;});
    if(it==automationRules_.end()) return false;
    automationRules_.erase(it);
    return true;
}

SurfaceAutomationRule* SurfaceInfrastructure::findAutomationRule(std::uint64_t stableId) {
    const auto it=std::find_if(automationRules_.begin(),automationRules_.end(),[&](const auto& r){return r.stableId==stableId;});
    return it==automationRules_.end()?nullptr:&*it;
}

const SurfaceAutomationRule* SurfaceInfrastructure::findAutomationRule(std::uint64_t stableId) const {
    const auto it=std::find_if(automationRules_.begin(),automationRules_.end(),[&](const auto& r){return r.stableId==stableId;});
    return it==automationRules_.end()?nullptr:&*it;
}

void SurfaceInfrastructure::evaluateAutomation(PlanetSurface& planet) {
    std::vector<SurfaceAutomationRule*> rules;
    rules.reserve(automationRules_.size());
    for(auto& r:automationRules_) if(r.enabled) rules.push_back(&r);
    std::sort(rules.begin(),rules.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    for(auto* rule:rules) {
        ++defenseTelemetry_.automationRulesEvaluated;
        const auto* controller=find(rule->controllerMachineId);
        const auto* source=find(rule->sourceStableId);
        if(!controller || !controller->enabled || !controller->powered || !source) continue;
        bool triggered=false;
        switch(rule->trigger) {
            case SurfaceAutomationTrigger::HostilesDetected:
                triggered=source->type==MachineType::SensorMast && static_cast<float>(source->detectedHostiles)>rule->threshold;
                break;
            case SurfaceAutomationTrigger::BatteryBelow:
                triggered=source->type==MachineType::BatteryBank && (source->storedEnergy/kBatteryCapacity)<rule->threshold;
                break;
            case SurfaceAutomationTrigger::AtmospherePressureBelow:
                triggered=source->type==MachineType::AtmosphereUnit && source->roomPressure<rule->threshold;
                break;
        }
        if(!triggered) continue;
        bool applied=false;
        switch(rule->action) {
            case SurfaceAutomationAction::EnableMachine:
                if(auto* target=find(rule->targetStableId)) { if(!target->enabled){target->enabled=true;applied=true;} }
                break;
            case SurfaceAutomationAction::DisableMachine:
                if(auto* target=find(rule->targetStableId)) { if(target->enabled){target->enabled=false;applied=true;} }
                break;
            case SurfaceAutomationAction::ClosePortal:
                if(auto* portal=findPortal(rule->targetStableId)) { if(portal->open) applied=setPortalOpen(planet,portal->stableId,false); }
                break;
        }
        if(applied) ++defenseTelemetry_.automationActionsApplied;
    }
}

void SurfaceInfrastructure::updateAirlocks(PlanetSurface& planet,float dt) {
    for(auto& assembly:airlocks_) {
        auto* controller=find(assembly.controllerMachineId);
        auto* inner=findPortal(assembly.innerPortalId);
        auto* outer=findPortal(assembly.outerPortalId);
        const bool valid=controller && controller->type==MachineType::AirlockController && inner && outer &&
                         inner->type==SurfacePortalType::Airlock && outer->type==SurfacePortalType::Airlock;
        if(!valid) {
            if(inner) setPortalOpenUnchecked(planet,inner->stableId,false);
            if(outer) setPortalOpenUnchecked(planet,outer->stableId,false);
            assembly.state=SurfaceAirlockState::Fault;
            continue;
        }

        // Hard invariant: an interlocked assembly may never expose both sides.
        if(inner->open && outer->open) {
            setPortalOpenUnchecked(planet,inner->stableId,false);
            setPortalOpenUnchecked(planet,outer->stableId,false);
            assembly.state=SurfaceAirlockState::Fault;
            continue;
        }

        if(!controller->enabled || !controller->powered) {
            // Fail closed on brownout. Preserve chamber gas state so cycling can
            // safely resume when power is restored.
            setPortalOpenUnchecked(planet,inner->stableId,false);
            setPortalOpenUnchecked(planet,outer->stableId,false);
            continue;
        }

        switch(assembly.state) {
            case SurfaceAirlockState::Idle:
                setPortalOpenUnchecked(planet,inner->stableId,false);
                setPortalOpenUnchecked(planet,outer->stableId,false);
                break;
            case SurfaceAirlockState::Depressurizing:
                setPortalOpenUnchecked(planet,inner->stableId,false);
                setPortalOpenUnchecked(planet,outer->stableId,false);
                assembly.chamberPressure=std::max(0.0f,assembly.chamberPressure-kAirlockDepressurizeRate*dt);
                assembly.chamberOxygen=std::max(0.0f,assembly.chamberOxygen-kAirlockDeoxygenateRate*dt);
                if(assembly.chamberPressure<=0.05f) {
                    assembly.chamberPressure=0.0f;
                    assembly.chamberOxygen=0.0f;
                    setPortalOpenUnchecked(planet,outer->stableId,true);
                    assembly.state=SurfaceAirlockState::ExteriorOpen;
                }
                break;
            case SurfaceAirlockState::ExteriorOpen:
                setPortalOpenUnchecked(planet,inner->stableId,false);
                setPortalOpenUnchecked(planet,outer->stableId,true);
                assembly.chamberPressure=0.0f;
                assembly.chamberOxygen=0.0f;
                break;
            case SurfaceAirlockState::Pressurizing:
                setPortalOpenUnchecked(planet,inner->stableId,false);
                setPortalOpenUnchecked(planet,outer->stableId,false);
                assembly.chamberPressure=std::min(1.0f,assembly.chamberPressure+kAirlockPressurizeRate*dt);
                assembly.chamberOxygen=std::min(assembly.chamberPressure,assembly.chamberOxygen+kAirlockOxygenateRate*dt);
                if(assembly.chamberPressure>=0.95f && assembly.chamberOxygen>=0.75f) {
                    assembly.chamberPressure=1.0f;
                    assembly.chamberOxygen=std::max(0.75f,assembly.chamberOxygen);
                    setPortalOpenUnchecked(planet,inner->stableId,true);
                    assembly.state=SurfaceAirlockState::InteriorOpen;
                }
                break;
            case SurfaceAirlockState::InteriorOpen:
                setPortalOpenUnchecked(planet,outer->stableId,false);
                setPortalOpenUnchecked(planet,inner->stableId,true);
                assembly.chamberPressure=std::max(assembly.chamberPressure,0.95f);
                assembly.chamberOxygen=std::max(assembly.chamberOxygen,0.75f);
                break;
            case SurfaceAirlockState::Fault:
                setPortalOpenUnchecked(planet,inner->stableId,false);
                setPortalOpenUnchecked(planet,outer->stableId,false);
                break;
        }
    }
}

void SurfaceInfrastructure::update(PlanetSurface& planet, float dt) {
    summary_={};
    for(auto& o:objects_) {
        o.powered=false;
        o.powerNetworkId=0;
        o.detectedHostiles=0;
        o.cooldownSeconds=std::max(0.0f,o.cooldownSeconds-dt);
    }

    if(!objects_.empty()) {
        Dsu dsu(static_cast<int>(objects_.size()));
        const float linkSq=kLinkRadius*kLinkRadius;
        for(int i=0;i<static_cast<int>(objects_.size());++i) {
            const Vec3 a=planet.cellCenterPosition(objects_[static_cast<std::size_t>(i)].anchor);
            for(int j=i+1;j<static_cast<int>(objects_.size());++j) {
                const Vec3 b=planet.cellCenterPosition(objects_[static_cast<std::size_t>(j)].anchor);
                if(lengthSq(a-b)<=linkSq) dsu.join(i,j);
            }
        }

        std::unordered_map<int,std::vector<int>> networks;
        for(int i=0;i<static_cast<int>(objects_.size());++i) networks[dsu.find(i)].push_back(i);
        summary_.networkCount=static_cast<int>(networks.size());

        for(auto& [_,ids]:networks) {
            std::sort(ids.begin(),ids.end(),[&](int a,int b){return objects_[static_cast<std::size_t>(a)].stableId<objects_[static_cast<std::size_t>(b)].stableId;});
            const std::uint64_t networkId=objects_[static_cast<std::size_t>(ids.front())].stableId;
            for(const int id:ids) objects_[static_cast<std::size_t>(id)].powerNetworkId=networkId;
            float generation=0.0f;
            std::vector<int> batteries;
            std::vector<int> loads;
            for(const int id:ids) {
                auto& o=objects_[static_cast<std::size_t>(id)];
                if(!o.enabled) continue;
                if(o.type==MachineType::BurnerGenerator && o.fuelSeconds>0.0f) {
                    generation+=kBurnerGeneration;
                    o.powered=true;
                    o.fuelSeconds=std::max(0.0f,o.fuelSeconds-dt);
                } else if(o.type==MachineType::BatteryBank) batteries.push_back(id);
                else if(demandFor(o.type)>0.0f) loads.push_back(id);
                else o.powered=true;
            }
            std::sort(loads.begin(),loads.end(),[&](int a,int b){
                const auto& A=objects_[static_cast<std::size_t>(a)];
                const auto& B=objects_[static_cast<std::size_t>(b)];
                if(priorityFor(A.type)!=priorityFor(B.type)) return priorityFor(A.type)<priorityFor(B.type);
                return A.stableId<B.stableId;
            });

            float batteryAvailable=0.0f,batteryCapacity=0.0f;
            for(const int id:batteries) {
                auto& b=objects_[static_cast<std::size_t>(id)];
                batteryAvailable+=std::min(b.storedEnergy,kBatteryDischargeRate*dt);
                batteryCapacity+=kBatteryCapacity;
            }
            float available=generation+batteryAvailable;
            float demand=0.0f,supplied=0.0f;
            for(const int id:loads) {
                auto& o=objects_[static_cast<std::size_t>(id)];
                const float d=demandFor(o.type);
                demand+=d;
                if(available+1e-4f>=d){o.powered=true;available-=d;supplied+=d;++summary_.poweredLoads;}
                else ++summary_.shedLoads;
            }

            float batteryNeeded=std::max(0.0f,supplied-std::min(generation,supplied));
            for(const int id:batteries) {
                auto& b=objects_[static_cast<std::size_t>(id)];
                const float take=std::min({b.storedEnergy,kBatteryDischargeRate*dt,batteryNeeded});
                b.storedEnergy-=take; batteryNeeded-=take; b.powered=true;
            }
            float surplus=std::max(0.0f,generation-supplied);
            for(const int id:batteries) {
                auto& b=objects_[static_cast<std::size_t>(id)];
                const float put=std::min({kBatteryCapacity-b.storedEnergy,kBatteryChargeRate*dt,surplus});
                b.storedEnergy+=put; surplus-=put; b.powered=true;
            }
            float stored=0.0f;
            for(const int id:batteries) stored+=objects_[static_cast<std::size_t>(id)].storedEnergy;
            summary_.generation+=generation;
            summary_.demand+=demand;
            summary_.supplied+=supplied;
            summary_.batteryStored+=stored;
            summary_.batteryCapacity+=batteryCapacity;
        }
    }

    // Defense storage behaves like a powered capacitor: charge only while the
    // pylon is an energized defense load, but retain existing charge through a
    // brownout so the next powered frame does not invent/delete energy.
    for(auto& o:objects_) {
        if(o.type==MachineType::ShieldPylon && o.enabled && o.powered)
            o.shieldCharge=std::min(kShieldCapacity,o.shieldCharge+kShieldRechargeRate*dt);
    }

    // Bounded atmosphere simulation. We intentionally store one scalar state
    // per Atmosphere Unit rather than per gas cell. Geometry determines the
    // currently connected sealed volume; pressure/oxygen fill while powered,
    // remain physically present in an intact unpowered room, and vent quickly
    // through a breach/open portal.
    for(auto& o:objects_) {
        if(o.type!=MachineType::AtmosphereUnit) continue;
        const auto room=planet.sealedVolume(o.anchor,8192);
        o.roomSealed=room.sealed && !room.truncated;
        if(o.roomSealed) {
            if(o.enabled && o.powered) {
                o.roomPressure=std::min(1.0f,o.roomPressure+kPressurizeRate*dt);
                o.roomOxygen=std::min(o.roomPressure,o.roomOxygen+kOxygenateRate*dt);
            }
        } else {
            o.roomPressure=std::max(0.0f,o.roomPressure-kVentPressureRate*dt);
            o.roomOxygen=std::max(0.0f,o.roomOxygen-kVentOxygenRate*dt);
        }
    }

    updateAirlocks(planet,dt);
}


SurfaceDefenseTelemetry SurfaceInfrastructure::updateDefense(PlanetSurface& planet, float /*dt*/, const std::vector<SurfaceHostileContact>& contacts) {
    defenseTelemetry_={};
    turretFireRequests_.clear();

    std::unordered_map<std::uint64_t,std::vector<SurfaceHostileContact>> networkContacts;
    for(auto& machine:objects_) {
        if(machine.type!=MachineType::SensorMast || !machine.enabled || !machine.powered || machine.powerNetworkId==0) continue;
        ++defenseTelemetry_.poweredSensors;
        const Vec3 sensorPos=planet.cellCenterPosition(machine.anchor);
        auto& detected=networkContacts[machine.powerNetworkId];
        for(const auto& contact:contacts) {
            if(contact.stableId==0 || contact.health<=0.0f) continue;
            if(lengthSq(contact.position-sensorPos)>kSensorRange*kSensorRange) continue;
            if(std::none_of(detected.begin(),detected.end(),[&](const auto& c){return c.stableId==contact.stableId;})) detected.push_back(contact);
        }
        machine.detectedHostiles=static_cast<int>(detected.size());
        defenseTelemetry_.hostilesDetected=std::max(defenseTelemetry_.hostilesDetected,machine.detectedHostiles);
    }

    for(auto& machine:objects_) {
        if(machine.type==MachineType::ShieldPylon) {
            if(machine.enabled && machine.powered) ++defenseTelemetry_.poweredShields;
            defenseTelemetry_.shieldCharge+=machine.shieldCharge;
            defenseTelemetry_.shieldCapacity+=kShieldCapacity;
            continue;
        }
        if(machine.type!=MachineType::Turret || !machine.enabled || !machine.powered || machine.ammo<=0 || machine.cooldownSeconds>0.0f) continue;
        ++defenseTelemetry_.activeTurrets;
        const auto it=networkContacts.find(machine.powerNetworkId);
        if(it==networkContacts.end()) continue;
        const Vec3 turretPos=planet.cellCenterPosition(machine.anchor);
        const SurfaceHostileContact* best=nullptr;
        float bestSq=kTurretRange*kTurretRange;
        for(const auto& contact:it->second) {
            const float d=lengthSq(contact.position-turretPos);
            if(d<bestSq || (std::abs(d-bestSq)<1e-5f && best && contact.stableId<best->stableId)) {
                bestSq=d;best=&contact;
            }
        }
        if(!best) continue;
        turretFireRequests_.push_back({machine.stableId,best->stableId,kTurretDamage});
        --machine.ammo;
        machine.cooldownSeconds=kTurretCooldown;
    }

    std::sort(turretFireRequests_.begin(),turretFireRequests_.end(),[](const auto& a,const auto& b){
        if(a.turretStableId!=b.turretStableId) return a.turretStableId<b.turretStableId;
        return a.targetStableId<b.targetStableId;
    });
    defenseTelemetry_.fireRequests=static_cast<int>(turretFireRequests_.size());
    evaluateAutomation(planet);
    return defenseTelemetry_;
}

std::vector<SurfaceTurretFireRequest> SurfaceInfrastructure::consumeTurretFireRequests() {
    auto out=std::move(turretFireRequests_);
    turretFireRequests_.clear();
    return out;
}

float SurfaceInfrastructure::absorbShieldDamage(const PlanetSurface& planet, Vec3 protectedPosition, float incomingDamage) {
    float remaining=std::max(0.0f,incomingDamage);
    while(remaining>0.0f) {
        SurfaceMachineObject* best=nullptr;
        float bestSq=kShieldRadius*kShieldRadius;
        for(auto& machine:objects_) {
            if(machine.type!=MachineType::ShieldPylon || !machine.enabled || !machine.powered || machine.shieldCharge<=0.0f) continue;
            const float d=lengthSq(planet.cellCenterPosition(machine.anchor)-protectedPosition);
            if(d<=bestSq) {
                if(!best || d<bestSq || (std::abs(d-bestSq)<1e-5f && machine.stableId<best->stableId)) {best=&machine;bestSq=d;}
            }
        }
        if(!best) break;
        const float absorbed=std::min(best->shieldCharge,remaining);
        best->shieldCharge-=absorbed;
        remaining-=absorbed;
        if(absorbed<=0.0f) break;
    }
    return remaining;
}

SurfaceAtmosphereSample SurfaceInfrastructure::atmosphereAt(const PlanetSurface& planet,Vec3 position,int maxRoomCells) const {
    SurfaceAtmosphereSample best{};
    const auto target=planet.locate(position);
    if(!planet.radialInBounds(target.radial) || planet.solidAt(position)) return best;
    const int targetIndex=planet.flatIndex(target);

    for(const auto& assembly:airlocks_) {
        if(lengthSq(planet.cellCenterPosition(assembly.chamberAnchor)-position)<=0.75f*0.75f) {
            SurfaceAtmosphereSample sample{};
            sample.pressure=assembly.chamberPressure;
            sample.oxygen=assembly.chamberOxygen;
            sample.airlockChamber=true;
            if(const auto* controller=find(assembly.controllerMachineId)) sample.poweredSource=controller->enabled && controller->powered;
            sample.sealed=assembly.state!=SurfaceAirlockState::ExteriorOpen;
            best=sample;
        }
    }

    for(const auto& o:objects_) {
        if(o.type!=MachineType::AtmosphereUnit || o.roomPressure<=0.0f) continue;
        const auto room=planet.sealedVolume(o.anchor,maxRoomCells);
        if(!room.sealed || room.truncated) continue;
        if(std::find(room.cells.begin(),room.cells.end(),targetIndex)==room.cells.end()) continue;
        SurfaceAtmosphereSample sample{o.roomPressure,o.roomOxygen,true,o.enabled&&o.powered,false};
        if(sample.oxygen>best.oxygen || (sample.oxygen==best.oxygen && sample.pressure>best.pressure)) best=sample;
    }
    return best;
}

bool SurfaceInfrastructure::oxygenatedAt(const PlanetSurface& planet, Vec3 position, int maxRoomCells) const {
    return atmosphereAt(planet,position,maxRoomCells).breathable();
}

} // namespace elysium
