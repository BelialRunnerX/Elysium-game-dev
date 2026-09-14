#include "world/InfrastructureJournal.hpp"
#include "world/Block.hpp"
#include "world/SurfaceIndustry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace elysium {
namespace {

bool validFace(CubeFace face) {
    const int value = static_cast<int>(face);
    return value >= static_cast<int>(CubeFace::PositiveX) &&
           value <= static_cast<int>(CubeFace::NegativeZ);
}

bool validKind(InfrastructureRecordKind kind) {
    const int value = static_cast<int>(kind);
    return value >= static_cast<int>(InfrastructureRecordKind::Machine) &&
           value <= static_cast<int>(InfrastructureRecordKind::AutomationRule);
}

bool sameAddress(const SurfaceCellAddress& a, const SurfaceCellAddress& b) {
    return a == b;
}

bool validMachineType(MachineType type) {
    const int value=static_cast<int>(type);
    return value>=static_cast<int>(MachineType::BurnerGenerator) &&
           value<=static_cast<int>(MachineType::Extractor);
}

bool validPortalType(SurfacePortalType type) {
    const int value=static_cast<int>(type);
    return value>=static_cast<int>(SurfacePortalType::Door) &&
           value<=static_cast<int>(SurfacePortalType::Airlock);
}

bool validAirlockState(SurfaceAirlockState state) {
    const int value=static_cast<int>(state);
    return value>=static_cast<int>(SurfaceAirlockState::Idle) &&
           value<=static_cast<int>(SurfaceAirlockState::Fault);
}

bool validAutomationTrigger(SurfaceAutomationTrigger trigger) {
    const int value=static_cast<int>(trigger);
    return value>=static_cast<int>(SurfaceAutomationTrigger::HostilesDetected) &&
           value<=static_cast<int>(SurfaceAutomationTrigger::AtmospherePressureBelow);
}

bool validAutomationAction(SurfaceAutomationAction action) {
    const int value=static_cast<int>(action);
    return value>=static_cast<int>(SurfaceAutomationAction::EnableMachine) &&
           value<=static_cast<int>(SurfaceAutomationAction::ClosePortal);
}

bool addressLess(const SurfaceCellAddress& a, const SurfaceCellAddress& b) {
    return std::tie(a.face,a.u,a.v,a.radial) < std::tie(b.face,b.u,b.v,b.radial);
}

bool finiteNonNegative(float v) {
    return std::isfinite(v) && v >= 0.0f;
}

int dependencyRank(InfrastructureRecordKind kind) {
    switch (kind) {
        case InfrastructureRecordKind::Machine: return 0;
        case InfrastructureRecordKind::Portal: return 1;
        case InfrastructureRecordKind::AirlockAssembly: return 2;
        case InfrastructureRecordKind::AutomationRule: return 3;
    }
    return 99;
}

void setError(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool validateMachine(const SurfaceMachineObject& object, const SurfaceCellAddress& owner, std::string* error) {
    if (object.stableId == 0) { setError(error,"machine stable ID is zero"); return false; }
    if (!validMachineType(object.type)) { setError(error,"machine type is invalid"); return false; }
    if (!sameAddress(object.anchor,owner)) { setError(error,"machine owner address does not match anchor"); return false; }
    if (!finiteNonNegative(object.fuelSeconds) || !finiteNonNegative(object.storedEnergy) ||
        !std::isfinite(object.roomPressure) || !std::isfinite(object.roomOxygen) ||
        object.roomPressure < 0.0f || object.roomPressure > 1.0f ||
        object.roomOxygen < 0.0f || object.roomOxygen > 1.0f ||
        object.ammo < 0 || !finiteNonNegative(object.shieldCharge) ||
        !finiteNonNegative(object.processProgressSeconds) || !finiteNonNegative(object.extractorProgressSeconds) || object.selectedRecipeId < 0 ||
        object.activeRecipeId < 0 || object.sorterFilterItemId < 0) {
        setError(error,"machine persistent state is invalid");
        return false;
    }
    if (object.inventory.size() > static_cast<std::size_t>(SurfaceIndustrySystem::MaxNetworkStorageStacks)) {
        setError(error,"machine inventory exceeds persistent stack budget");
        return false;
    }
    int previousItem=0;
    for (const auto& stack:object.inventory) {
        const bool blockItem=stack.itemId>0 && stack.itemId<kBlockTypeCount;
        const bool industryItem=validIndustryItemId(stack.itemId);
        if ((!blockItem && !industryItem) || stack.count<=0 || stack.count>SurfaceIndustrySystem::MaxStackCount ||
            stack.itemId<=previousItem) {
            setError(error,"machine inventory is not normalized");
            return false;
        }
        previousItem=stack.itemId;
    }

    if (!finiteNonNegative(object.logisticsProgressSeconds)) {
        setError(error,"machine logistics progress is invalid");
        return false;
    }
    const bool transport=object.type==MachineType::Conveyor || object.type==MachineType::Sorter || object.type==MachineType::CargoLoader;
    const bool hasAnyLink=object.logisticsSourceStableId!=0 || object.logisticsTargetStableId!=0 || object.logisticsAlternateTargetStableId!=0;
    if (!transport && (hasAnyLink || object.logisticsProgressSeconds>0.0f)) {
        setError(error,"non-logistics machine carries logistics link state");
        return false;
    }
    if (transport && hasAnyLink) {
        if (object.logisticsSourceStableId==0 || object.logisticsTargetStableId==0 ||
            object.logisticsSourceStableId==object.stableId || object.logisticsTargetStableId==object.stableId ||
            object.logisticsSourceStableId==object.logisticsTargetStableId) {
            setError(error,"logistics machine has invalid source/target identities");
            return false;
        }
        if (object.type==MachineType::Sorter) {
            const bool validFilter=(object.sorterFilterItemId>0 && object.sorterFilterItemId<kBlockTypeCount) ||
                validIndustryItemId(object.sorterFilterItemId);
            if (object.logisticsAlternateTargetStableId==0 ||
                object.logisticsAlternateTargetStableId==object.stableId ||
                object.logisticsAlternateTargetStableId==object.logisticsSourceStableId ||
                object.logisticsAlternateTargetStableId==object.logisticsTargetStableId || !validFilter) {
                setError(error,"sorter logistics alternate/filter state is invalid");
                return false;
            }
        } else if (object.logisticsAlternateTargetStableId!=0 || object.sorterFilterItemId!=0) {
            setError(error,"non-sorter logistics machine carries sorter state");
            return false;
        }
    } else if (transport && (object.logisticsProgressSeconds>0.0f || object.sorterFilterItemId!=0)) {
        setError(error,"unconfigured logistics machine carries active routing state");
        return false;
    }
    return true;
}

bool validatePortal(const SurfacePortalObject& object, const SurfaceCellAddress& owner, std::string* error) {
    if (object.stableId == 0) { setError(error,"portal stable ID is zero"); return false; }
    if (!validPortalType(object.type)) { setError(error,"portal type is invalid"); return false; }
    if (!sameAddress(object.anchor,owner)) { setError(error,"portal owner address does not match anchor"); return false; }
    return true;
}

bool validateAirlock(const SurfaceAirlockAssembly& object, const SurfaceCellAddress& owner, std::string* error) {
    if (object.stableId == 0 || object.controllerMachineId == 0 || object.innerPortalId == 0 || object.outerPortalId == 0) {
        setError(error,"airlock stable/dependency ID is zero");
        return false;
    }
    if (object.innerPortalId == object.outerPortalId) { setError(error,"airlock portals are identical"); return false; }
    if (!validAirlockState(object.state)) { setError(error,"airlock state is invalid"); return false; }
    if (!sameAddress(object.chamberAnchor,owner)) { setError(error,"airlock owner address does not match chamber anchor"); return false; }
    if (!std::isfinite(object.chamberPressure) || !std::isfinite(object.chamberOxygen) ||
        object.chamberPressure < 0.0f || object.chamberPressure > 1.0f ||
        object.chamberOxygen < 0.0f || object.chamberOxygen > 1.0f) {
        setError(error,"airlock chamber state is invalid");
        return false;
    }
    return true;
}

bool validateAutomation(const SurfaceAutomationRule& object, std::uint64_t stableId, std::string* error) {
    if (object.stableId == 0 || object.stableId != stableId || object.controllerMachineId == 0 ||
        object.sourceStableId == 0 || object.targetStableId == 0) {
        setError(error,"automation stable/controller/source/target ID is invalid");
        return false;
    }
    if (!validAutomationTrigger(object.trigger) || !validAutomationAction(object.action)) {
        setError(error,"automation trigger/action is invalid"); return false;
    }
    if (!finiteNonNegative(object.threshold)) { setError(error,"automation threshold is invalid"); return false; }
    return true;
}

bool validateRef(const InfrastructureStableRef& ref, std::string* error) {
    if (!validKind(ref.kind)) { setError(error,"infrastructure dependency kind is invalid"); return false; }
    if (ref.stableId == 0) { setError(error,"infrastructure dependency stable ID is zero"); return false; }
    if (!validFace(ref.ownerAddress.face)) { setError(error,"infrastructure dependency owner face is invalid"); return false; }
    return true;
}

using RefKey = std::pair<int,std::uint64_t>;
RefKey refKey(InfrastructureRecordKind kind, std::uint64_t stableId) {
    return {static_cast<int>(kind),stableId};
}
RefKey refKey(const InfrastructureStableRef& ref) { return refKey(ref.kind,ref.stableId); }
RefKey recordKey(const InfrastructureJournalRecord& record) { return refKey(record.kind,record.stableId); }

std::vector<RefKey> expectedDependencyKeys(const InfrastructureJournalRecord& record) {
    std::vector<RefKey> expected;
    if (record.op != InfrastructureJournalOp::Upsert || !record.payload) return expected;
    if (record.kind == InfrastructureRecordKind::Machine && record.schemaVersion>=4) {
        const auto* object=std::get_if<SurfaceMachineObject>(&*record.payload);
        if (!object) return expected;
        if (object->logisticsSourceStableId) expected.push_back(refKey(InfrastructureRecordKind::Machine,object->logisticsSourceStableId));
        if (object->logisticsTargetStableId) expected.push_back(refKey(InfrastructureRecordKind::Machine,object->logisticsTargetStableId));
        if (object->logisticsAlternateTargetStableId) expected.push_back(refKey(InfrastructureRecordKind::Machine,object->logisticsAlternateTargetStableId));
    } else if (record.kind == InfrastructureRecordKind::AirlockAssembly) {
        const auto* object=std::get_if<SurfaceAirlockAssembly>(&*record.payload);
        if (!object) return expected;
        expected.push_back(refKey(InfrastructureRecordKind::Machine,object->controllerMachineId));
        expected.push_back(refKey(InfrastructureRecordKind::Portal,object->innerPortalId));
        expected.push_back(refKey(InfrastructureRecordKind::Portal,object->outerPortalId));
    } else if (record.kind == InfrastructureRecordKind::AutomationRule) {
        const auto* object=std::get_if<SurfaceAutomationRule>(&*record.payload);
        if (!object) return expected;
        expected.push_back(refKey(InfrastructureRecordKind::Machine,object->controllerMachineId));
        expected.push_back(refKey(InfrastructureRecordKind::Machine,object->sourceStableId));
        expected.push_back(refKey(object->action==SurfaceAutomationAction::ClosePortal
                                  ? InfrastructureRecordKind::Portal
                                  : InfrastructureRecordKind::Machine,
                                  object->targetStableId));
    }
    std::sort(expected.begin(),expected.end());
    expected.erase(std::unique(expected.begin(),expected.end()),expected.end());
    return expected;
}

bool validateDependencySyntax(const InfrastructureJournalRecord& record, std::string* error) {
    if (record.schemaVersion < 2) {
        if (!record.dependencies.empty()) {
            setError(error,"schema-v1 infrastructure record unexpectedly carries dependencies");
            return false;
        }
        return true;
    }

    std::set<RefKey> seen;
    for (const auto& ref:record.dependencies) {
        if (!validateRef(ref,error)) return false;
        if (!seen.insert(refKey(ref)).second) {
            setError(error,"infrastructure record has duplicate dependency identity");
            return false;
        }
    }

    const auto expected=expectedDependencyKeys(record);
    std::vector<RefKey> actual;
    actual.reserve(record.dependencies.size());
    for (const auto& ref:record.dependencies) actual.push_back(refKey(ref));
    std::sort(actual.begin(),actual.end());
    if (actual != expected) {
        setError(error,"infrastructure record dependency set does not match payload stable references");
        return false;
    }
    return true;
}

InfrastructureStableRef machineRef(const SurfaceMachineObject& object) {
    return {InfrastructureRecordKind::Machine,object.stableId,object.anchor};
}
InfrastructureStableRef portalRef(const SurfacePortalObject& object) {
    return {InfrastructureRecordKind::Portal,object.stableId,object.anchor};
}

bool deterministicRecordLess(const InfrastructureJournalRecord& a, const InfrastructureJournalRecord& b) {
    if (a.op != b.op) return a.op == InfrastructureJournalOp::Upsert;
    const int ar=dependencyRank(a.kind), br=dependencyRank(b.kind);
    if (ar!=br) {
        if (a.op==InfrastructureJournalOp::Upsert) return ar<br;
        return ar>br;
    }
    if (a.stableId!=b.stableId) return a.stableId<b.stableId;
    return addressLess(a.ownerAddress,b.ownerAddress);
}

bool dependencyPresent(const SurfaceInfrastructure& infrastructure,const InfrastructureStableRef& ref) {
    switch(ref.kind) {
        case InfrastructureRecordKind::Machine: {
            const auto* object=infrastructure.find(ref.stableId);
            return object && object->anchor==ref.ownerAddress;
        }
        case InfrastructureRecordKind::Portal: {
            const auto* object=infrastructure.findPortal(ref.stableId);
            return object && object->anchor==ref.ownerAddress;
        }
        case InfrastructureRecordKind::AirlockAssembly: {
            const auto* object=infrastructure.findAirlockAssembly(ref.stableId);
            return object && object->chamberAnchor==ref.ownerAddress;
        }
        case InfrastructureRecordKind::AutomationRule:
            return infrastructure.findAutomationRule(ref.stableId)!=nullptr;
    }
    return false;
}

bool allDependenciesPresent(const SurfaceInfrastructure& infrastructure,const InfrastructureJournalRecord& record) {
    // Machine-to-machine logistics refs are graph edges, not constructor
    // prerequisites. Belt loops are legal, so Machine records may restore
    // before their peer machines; closure/frontier validation handles missing
    // sparse shards. Airlocks and automation still require live dependencies
    // because their restore APIs validate those object types immediately.
    if(record.kind==InfrastructureRecordKind::Machine) return true;
    return std::all_of(record.dependencies.begin(),record.dependencies.end(),[&](const auto& ref){return dependencyPresent(infrastructure,ref);});
}

bool applyOneUpsert(SurfaceInfrastructure& infrastructure,
                    const InfrastructureJournalRecord& record,
                    std::string* error) {
    // Dependency locators double as load-order preconditions for records whose
    // constructor requires live peers. Machine logistics references are allowed
    // to form cycles and are therefore validated as snapshot/frontier edges, not
    // as Machine restore prerequisites.
    if(!allDependenciesPresent(infrastructure,record)) {
        setError(error,"infrastructure upsert has unresolved cross-shard dependency; load dependency ownerAddress first");
        return false;
    }

    switch (record.kind) {
        case InfrastructureRecordKind::Machine: {
            const auto* object = std::get_if<SurfaceMachineObject>(&*record.payload);
            if (!object || !infrastructure.restore(*object)) {
                setError(error,"failed to restore machine infrastructure record");
                return false;
            }
            return true;
        }
        case InfrastructureRecordKind::Portal: {
            const auto* object = std::get_if<SurfacePortalObject>(&*record.payload);
            if (!object || !infrastructure.restorePortal(*object)) {
                setError(error,"failed to restore portal infrastructure record");
                return false;
            }
            return true;
        }
        case InfrastructureRecordKind::AirlockAssembly: {
            const auto* object = std::get_if<SurfaceAirlockAssembly>(&*record.payload);
            if (!object || !infrastructure.restoreAirlockAssembly(*object)) {
                setError(error,"failed to restore airlock assembly infrastructure record");
                return false;
            }
            return true;
        }
        case InfrastructureRecordKind::AutomationRule: {
            const auto* object = std::get_if<SurfaceAutomationRule>(&*record.payload);
            if (!object || !infrastructure.restoreAutomationRule(*object)) {
                setError(error,"failed to restore automation infrastructure record");
                return false;
            }
            return true;
        }
    }
    setError(error,"unknown infrastructure record kind");
    return false;
}

bool applyOneTombstone(SurfaceInfrastructure& infrastructure,
                       PlanetSurface& planet,
                       const InfrastructureJournalRecord& record,
                       std::string* error) {
    bool exists=false;
    bool removed=false;
    switch (record.kind) {
        case InfrastructureRecordKind::AutomationRule:
            exists=infrastructure.findAutomationRule(record.stableId)!=nullptr;
            removed=!exists || infrastructure.removeAutomationRule(record.stableId);
            break;
        case InfrastructureRecordKind::AirlockAssembly:
            exists=infrastructure.findAirlockAssembly(record.stableId)!=nullptr;
            removed=!exists || infrastructure.removeAirlockAssembly(record.stableId);
            break;
        case InfrastructureRecordKind::Portal:
            exists=infrastructure.findPortal(record.stableId)!=nullptr;
            // The ordinary voxel journal owns the cell. Removing the stable
            // object record must not erase/alter the authored voxel state.
            removed=!exists || infrastructure.removePortal(planet,record.stableId,false);
            break;
        case InfrastructureRecordKind::Machine:
            exists=infrastructure.find(record.stableId)!=nullptr;
            removed=!exists || infrastructure.remove(record.stableId);
            break;
    }
    if (!removed) {
        setError(error,"failed to apply infrastructure tombstone because surviving dependents still reference the object");
        return false;
    }
    return true;
}

} // namespace

const char* infrastructureRecordKindName(InfrastructureRecordKind kind) {
    switch(kind) {
        case InfrastructureRecordKind::Machine: return "Machine";
        case InfrastructureRecordKind::Portal: return "Portal";
        case InfrastructureRecordKind::AirlockAssembly: return "AirlockAssembly";
        case InfrastructureRecordKind::AutomationRule: return "AutomationRule";
    }
    return "Unknown";
}

const char* infrastructureJournalOpName(InfrastructureJournalOp op) {
    switch(op) {
        case InfrastructureJournalOp::Upsert: return "Upsert";
        case InfrastructureJournalOp::Tombstone: return "Tombstone";
    }
    return "Unknown";
}

InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceMachineObject& object,
                                                       std::vector<InfrastructureStableRef> dependencies) {
    InfrastructureJournalRecord record{};
    record.kind=InfrastructureRecordKind::Machine;
    record.stableId=object.stableId;
    record.ownerAddress=object.anchor;
    record.dependencies=std::move(dependencies);
    std::sort(record.dependencies.begin(),record.dependencies.end(),[](const auto& a,const auto& b){return refKey(a)<refKey(b);});
    record.dependencies.erase(std::unique(record.dependencies.begin(),record.dependencies.end(),[](const auto& a,const auto& b){return refKey(a)==refKey(b);}),record.dependencies.end());
    record.payload=InfrastructureRecordPayload{object};
    return record;
}

InfrastructureJournalRecord makeInfrastructureUpsert(const SurfacePortalObject& object) {
    InfrastructureJournalRecord record{};
    record.kind=InfrastructureRecordKind::Portal;
    record.stableId=object.stableId;
    record.ownerAddress=object.anchor;
    record.payload=InfrastructureRecordPayload{object};
    return record;
}

InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceAirlockAssembly& object,
                                                       InfrastructureStableRef controller,
                                                       InfrastructureStableRef innerPortal,
                                                       InfrastructureStableRef outerPortal) {
    InfrastructureJournalRecord record{};
    record.kind=InfrastructureRecordKind::AirlockAssembly;
    record.stableId=object.stableId;
    record.ownerAddress=object.chamberAnchor;
    record.dependencies={controller,innerPortal,outerPortal};
    record.payload=InfrastructureRecordPayload{object};
    return record;
}

InfrastructureJournalRecord makeInfrastructureUpsert(const SurfaceAutomationRule& object,
                                                       SurfaceCellAddress controllerAddress,
                                                       InfrastructureStableRef controller,
                                                       InfrastructureStableRef source,
                                                       InfrastructureStableRef target) {
    InfrastructureJournalRecord record{};
    record.kind=InfrastructureRecordKind::AutomationRule;
    record.stableId=object.stableId;
    record.ownerAddress=controllerAddress;
    record.dependencies={controller,source,target};
    std::sort(record.dependencies.begin(),record.dependencies.end(),[](const auto& a,const auto& b){return refKey(a)<refKey(b);});
    record.dependencies.erase(std::unique(record.dependencies.begin(),record.dependencies.end(),[](const auto& a,const auto& b){return refKey(a)==refKey(b);}),record.dependencies.end());
    record.payload=InfrastructureRecordPayload{object};
    return record;
}

InfrastructureJournalRecord makeInfrastructureTombstone(InfrastructureRecordKind kind,
                                                         std::uint64_t stableId,
                                                         SurfaceCellAddress ownerAddress) {
    InfrastructureJournalRecord record{};
    record.op=InfrastructureJournalOp::Tombstone;
    record.kind=kind;
    record.stableId=stableId;
    record.ownerAddress=ownerAddress;
    return record;
}

bool validateInfrastructureJournalRecord(const InfrastructureJournalRecord& record,
                                         std::string* error) {
    if (record.schemaVersion < InfrastructureJournalRecord::OldestSupportedSchemaVersion ||
        record.schemaVersion > InfrastructureJournalRecord::SchemaVersion) {
        setError(error,"unsupported infrastructure record schema");
        return false;
    }
    if (!validKind(record.kind)) { setError(error,"infrastructure record kind is invalid"); return false; }
    if (record.stableId == 0) { setError(error,"infrastructure stable ID is zero"); return false; }
    if (!validFace(record.ownerAddress.face)) { setError(error,"infrastructure owner face is invalid"); return false; }

    if (record.op == InfrastructureJournalOp::Tombstone) {
        if (record.payload) { setError(error,"tombstone unexpectedly carries payload"); return false; }
        if (!record.dependencies.empty()) { setError(error,"tombstone unexpectedly carries dependencies"); return false; }
        return true;
    }
    if (record.op != InfrastructureJournalOp::Upsert) { setError(error,"unknown infrastructure journal operation"); return false; }
    if (!record.payload) { setError(error,"upsert is missing payload"); return false; }

    bool payloadValid=false;
    switch(record.kind) {
        case InfrastructureRecordKind::Machine: {
            const auto* object=std::get_if<SurfaceMachineObject>(&*record.payload);
            if (!object || object->stableId!=record.stableId) { setError(error,"machine payload kind/ID mismatch"); return false; }
            payloadValid=validateMachine(*object,record.ownerAddress,error);
            break;
        }
        case InfrastructureRecordKind::Portal: {
            const auto* object=std::get_if<SurfacePortalObject>(&*record.payload);
            if (!object || object->stableId!=record.stableId) { setError(error,"portal payload kind/ID mismatch"); return false; }
            payloadValid=validatePortal(*object,record.ownerAddress,error);
            break;
        }
        case InfrastructureRecordKind::AirlockAssembly: {
            const auto* object=std::get_if<SurfaceAirlockAssembly>(&*record.payload);
            if (!object || object->stableId!=record.stableId) { setError(error,"airlock payload kind/ID mismatch"); return false; }
            payloadValid=validateAirlock(*object,record.ownerAddress,error);
            break;
        }
        case InfrastructureRecordKind::AutomationRule: {
            const auto* object=std::get_if<SurfaceAutomationRule>(&*record.payload);
            if (!object) { setError(error,"automation payload kind mismatch"); return false; }
            payloadValid=validateAutomation(*object,record.stableId,error);
            break;
        }
    }
    if (!payloadValid) return false;
    return validateDependencySyntax(record,error);
}

std::string serializeInfrastructureJournalRecord(const InfrastructureJournalRecord& record) {
    std::string error;
    if (!validateInfrastructureJournalRecord(record,&error)) return {};

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"ELYSIUM_INFRA_RECORD "<<record.schemaVersion<<"\n";
    out<<"header "<<static_cast<int>(record.op)<<' '<<static_cast<int>(record.kind)<<' '<<record.stableId<<"\n";
    out<<"owner "<<static_cast<int>(record.ownerAddress.face)<<' '<<record.ownerAddress.u<<' '<<record.ownerAddress.v<<' '<<record.ownerAddress.radial<<"\n";
    if (record.schemaVersion >= 2) {
        out<<"deps "<<record.dependencies.size()<<"\n";
        std::vector<InfrastructureStableRef> deps=record.dependencies;
        std::sort(deps.begin(),deps.end(),[](const auto& a,const auto& b){
            if (refKey(a)!=refKey(b)) return refKey(a)<refKey(b);
            return addressLess(a.ownerAddress,b.ownerAddress);
        });
        for (const auto& ref:deps) {
            out<<"dep "<<static_cast<int>(ref.kind)<<' '<<ref.stableId<<' '
               <<static_cast<int>(ref.ownerAddress.face)<<' '<<ref.ownerAddress.u<<' '
               <<ref.ownerAddress.v<<' '<<ref.ownerAddress.radial<<"\n";
        }
    }
    if (record.op == InfrastructureJournalOp::Tombstone) {
        out<<"END\n";
        return out.str();
    }

    switch(record.kind) {
        case InfrastructureRecordKind::Machine: {
            const auto& o=std::get<SurfaceMachineObject>(*record.payload);
            out<<"machine "<<static_cast<int>(o.type)<<' '<<(o.enabled?1:0)<<' '
               <<o.fuelSeconds<<' '<<o.storedEnergy<<' '<<o.roomPressure<<' '<<o.roomOxygen<<' '
               <<o.ammo<<' '<<o.shieldCharge;
            if(record.schemaVersion>=3) {
                out<<' '<<o.selectedRecipeId<<' '<<o.activeRecipeId<<' '<<o.processProgressSeconds<<' '<<o.sorterFilterItemId;
                if(record.schemaVersion>=4) {
                    out<<' '<<o.logisticsSourceStableId<<' '<<o.logisticsTargetStableId<<' '<<o.logisticsAlternateTargetStableId<<' '<<o.logisticsProgressSeconds;
                }
                if(record.schemaVersion>=5) out<<' '<<o.extractorProgressSeconds;
                out<<"\n";
                out<<"inventory "<<o.inventory.size()<<"\n";
                for(const auto& stack:o.inventory) out<<"item "<<stack.itemId<<' '<<stack.count<<"\n";
            } else out<<"\n";
            break;
        }
        case InfrastructureRecordKind::Portal: {
            const auto& o=std::get<SurfacePortalObject>(*record.payload);
            out<<"portal "<<static_cast<int>(o.type)<<' '<<(o.open?1:0)<<"\n";
            break;
        }
        case InfrastructureRecordKind::AirlockAssembly: {
            const auto& o=std::get<SurfaceAirlockAssembly>(*record.payload);
            out<<"airlock "<<o.controllerMachineId<<' '<<o.innerPortalId<<' '<<o.outerPortalId<<' '
               <<static_cast<int>(o.state)<<' '<<o.chamberPressure<<' '<<o.chamberOxygen<<"\n";
            break;
        }
        case InfrastructureRecordKind::AutomationRule: {
            const auto& o=std::get<SurfaceAutomationRule>(*record.payload);
            out<<"automation "<<o.controllerMachineId<<' '<<static_cast<int>(o.trigger)<<' '
               <<o.sourceStableId<<' '<<o.threshold<<' '<<static_cast<int>(o.action)<<' '
               <<o.targetStableId<<' '<<(o.enabled?1:0)<<"\n";
            break;
        }
    }
    out<<"END\n";
    return out.str();
}

std::optional<InfrastructureJournalRecord> deserializeInfrastructureJournalRecord(std::string_view text,
                                                                                   std::string* error) {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string tag;
    int schema{};
    if (!(in>>tag>>schema) || tag!="ELYSIUM_INFRA_RECORD") { setError(error,"invalid infrastructure record header"); return std::nullopt; }

    InfrastructureJournalRecord record{};
    record.schemaVersion=static_cast<std::uint32_t>(schema);
    int op{},kind{};
    if (!(in>>tag>>op>>kind>>record.stableId) || tag!="header") { setError(error,"invalid infrastructure record identity"); return std::nullopt; }
    record.op=static_cast<InfrastructureJournalOp>(op);
    record.kind=static_cast<InfrastructureRecordKind>(kind);
    int face{};
    if (!(in>>tag>>face>>record.ownerAddress.u>>record.ownerAddress.v>>record.ownerAddress.radial) || tag!="owner") {
        setError(error,"invalid infrastructure owner address"); return std::nullopt;
    }
    record.ownerAddress.face=static_cast<CubeFace>(face);

    if (record.schemaVersion >= 2) {
        std::size_t depCount{};
        if (!(in>>tag>>depCount) || tag!="deps") { setError(error,"missing infrastructure dependency header"); return std::nullopt; }
        record.dependencies.reserve(depCount);
        for (std::size_t i=0;i<depCount;++i) {
            InfrastructureStableRef ref{};
            int refKind{},refFace{};
            if (!(in>>tag>>refKind>>ref.stableId>>refFace>>ref.ownerAddress.u>>ref.ownerAddress.v>>ref.ownerAddress.radial) || tag!="dep") {
                setError(error,"invalid infrastructure dependency record"); return std::nullopt;
            }
            ref.kind=static_cast<InfrastructureRecordKind>(refKind);
            ref.ownerAddress.face=static_cast<CubeFace>(refFace);
            record.dependencies.push_back(ref);
        }
    }

    if (record.op == InfrastructureJournalOp::Tombstone) {
        if (!(in>>tag) || tag!="END") { setError(error,"invalid infrastructure tombstone terminator"); return std::nullopt; }
        if (!validateInfrastructureJournalRecord(record,error)) return std::nullopt;
        return record;
    }

    if (!(in>>tag)) { setError(error,"missing infrastructure payload"); return std::nullopt; }
    switch(record.kind) {
        case InfrastructureRecordKind::Machine: {
            if (tag!="machine") { setError(error,"machine payload tag mismatch"); return std::nullopt; }
            SurfaceMachineObject o{}; int type{},enabled{};
            o.stableId=record.stableId; o.anchor=record.ownerAddress;
            if (!(in>>type>>enabled>>o.fuelSeconds>>o.storedEnergy>>o.roomPressure>>o.roomOxygen>>o.ammo>>o.shieldCharge)) {
                setError(error,"invalid machine infrastructure payload"); return std::nullopt;
            }
            o.type=static_cast<MachineType>(type); o.enabled=enabled!=0;
            if(record.schemaVersion>=3) {
                if(!(in>>o.selectedRecipeId>>o.activeRecipeId>>o.processProgressSeconds>>o.sorterFilterItemId)) {
                    setError(error,"invalid industrial machine state"); return std::nullopt;
                }
                if(record.schemaVersion>=4) {
                    if(!(in>>o.logisticsSourceStableId>>o.logisticsTargetStableId>>o.logisticsAlternateTargetStableId>>o.logisticsProgressSeconds)) {
                        setError(error,"invalid logistics machine state"); return std::nullopt;
                    }
                }
                if(record.schemaVersion>=5 && !(in>>o.extractorProgressSeconds)) {
                    setError(error,"invalid extractor machine state"); return std::nullopt;
                }
                std::size_t itemCount{};
                if(!(in>>tag>>itemCount) || tag!="inventory" || itemCount>static_cast<std::size_t>(SurfaceIndustrySystem::MaxNetworkStorageStacks)) {
                    setError(error,"invalid machine inventory header"); return std::nullopt;
                }
                o.inventory.reserve(itemCount);
                for(std::size_t i=0;i<itemCount;++i) {
                    SurfaceItemStack stack{};
                    if(!(in>>tag>>stack.itemId>>stack.count) || tag!="item") {
                        setError(error,"invalid machine inventory entry"); return std::nullopt;
                    }
                    o.inventory.push_back(stack);
                }
            }
            record.payload=InfrastructureRecordPayload{o};
            break;
        }
        case InfrastructureRecordKind::Portal: {
            if (tag!="portal") { setError(error,"portal payload tag mismatch"); return std::nullopt; }
            SurfacePortalObject o{}; int type{},open{};
            o.stableId=record.stableId; o.anchor=record.ownerAddress;
            if (!(in>>type>>open)) { setError(error,"invalid portal infrastructure payload"); return std::nullopt; }
            o.type=static_cast<SurfacePortalType>(type); o.open=open!=0;
            record.payload=InfrastructureRecordPayload{o};
            break;
        }
        case InfrastructureRecordKind::AirlockAssembly: {
            if (tag!="airlock") { setError(error,"airlock payload tag mismatch"); return std::nullopt; }
            SurfaceAirlockAssembly o{}; int state{};
            o.stableId=record.stableId; o.chamberAnchor=record.ownerAddress;
            if (!(in>>o.controllerMachineId>>o.innerPortalId>>o.outerPortalId>>state>>o.chamberPressure>>o.chamberOxygen)) {
                setError(error,"invalid airlock infrastructure payload"); return std::nullopt;
            }
            o.state=static_cast<SurfaceAirlockState>(state);
            record.payload=InfrastructureRecordPayload{o};
            break;
        }
        case InfrastructureRecordKind::AutomationRule: {
            if (tag!="automation") { setError(error,"automation payload tag mismatch"); return std::nullopt; }
            SurfaceAutomationRule o{}; int trigger{},action{},enabled{};
            o.stableId=record.stableId;
            if (!(in>>o.controllerMachineId>>trigger>>o.sourceStableId>>o.threshold>>action>>o.targetStableId>>enabled)) {
                setError(error,"invalid automation infrastructure payload"); return std::nullopt;
            }
            o.trigger=static_cast<SurfaceAutomationTrigger>(trigger);
            o.action=static_cast<SurfaceAutomationAction>(action);
            o.enabled=enabled!=0;
            record.payload=InfrastructureRecordPayload{o};
            break;
        }
    }
    if (!(in>>tag) || tag!="END") { setError(error,"missing infrastructure record terminator"); return std::nullopt; }
    if (!validateInfrastructureJournalRecord(record,error)) return std::nullopt;
    return record;
}

bool captureInfrastructureUpserts(const SurfaceInfrastructure& infrastructure,
                                  std::vector<InfrastructureJournalRecord>& out,
                                  std::string* error) {
    out.clear();
    out.reserve(infrastructure.objects().size()+infrastructure.portals().size()+
                infrastructure.airlockAssemblies().size()+infrastructure.automationRules().size());

    for (const auto& object:infrastructure.objects()) {
        std::vector<InfrastructureStableRef> deps;
        if(object.logisticsSourceStableId) {
            const auto* dep=infrastructure.find(object.logisticsSourceStableId);
            if(!dep) { setError(error,"logistics machine has unresolved source during capture"); out.clear(); return false; }
            deps.push_back(machineRef(*dep));
        }
        if(object.logisticsTargetStableId) {
            const auto* dep=infrastructure.find(object.logisticsTargetStableId);
            if(!dep) { setError(error,"logistics machine has unresolved target during capture"); out.clear(); return false; }
            deps.push_back(machineRef(*dep));
        }
        if(object.logisticsAlternateTargetStableId) {
            const auto* dep=infrastructure.find(object.logisticsAlternateTargetStableId);
            if(!dep) { setError(error,"sorter has unresolved alternate target during capture"); out.clear(); return false; }
            deps.push_back(machineRef(*dep));
        }
        out.push_back(makeInfrastructureUpsert(object,std::move(deps)));
    }
    for (const auto& object:infrastructure.portals()) out.push_back(makeInfrastructureUpsert(object));

    for (const auto& object:infrastructure.airlockAssemblies()) {
        const auto* controller=infrastructure.find(object.controllerMachineId);
        const auto* inner=infrastructure.findPortal(object.innerPortalId);
        const auto* outer=infrastructure.findPortal(object.outerPortalId);
        if (!controller || !inner || !outer) {
            setError(error,"airlock assembly has unresolved stable dependencies during capture");
            out.clear();
            return false;
        }
        out.push_back(makeInfrastructureUpsert(object,machineRef(*controller),portalRef(*inner),portalRef(*outer)));
    }

    for (const auto& rule:infrastructure.automationRules()) {
        const auto* controller=infrastructure.find(rule.controllerMachineId);
        const auto* source=infrastructure.find(rule.sourceStableId);
        if (!controller || !source) { setError(error,"automation rule has unresolved controller/source during capture"); out.clear(); return false; }
        InfrastructureStableRef target{};
        if (rule.action==SurfaceAutomationAction::ClosePortal) {
            const auto* portal=infrastructure.findPortal(rule.targetStableId);
            if (!portal) { setError(error,"automation rule has unresolved portal target during capture"); out.clear(); return false; }
            target=portalRef(*portal);
        } else {
            const auto* machine=infrastructure.find(rule.targetStableId);
            if (!machine) { setError(error,"automation rule has unresolved machine target during capture"); out.clear(); return false; }
            target=machineRef(*machine);
        }
        out.push_back(makeInfrastructureUpsert(rule,controller->anchor,machineRef(*controller),machineRef(*source),target));
    }

    for (const auto& record:out) if (!validateInfrastructureJournalRecord(record,error)) { out.clear(); return false; }
    std::sort(out.begin(),out.end(),deterministicRecordLess);
    return true;
}

bool compactInfrastructureJournal(const std::vector<InfrastructureJournalRecord>& records,
                                  std::vector<InfrastructureJournalRecord>& out,
                                  InfrastructureCompactionPolicy policy,
                                  std::string* error) {
    out.clear();
    std::map<RefKey,InfrastructureJournalRecord> latest;
    std::map<RefKey,SurfaceCellAddress> immutableOwner;

    for (const auto& record:records) {
        if (!validateInfrastructureJournalRecord(record,error)) return false;
        const auto key=recordKey(record);
        const auto ownerIt=immutableOwner.find(key);
        if (ownerIt==immutableOwner.end()) immutableOwner.emplace(key,record.ownerAddress);
        else if (ownerIt->second != record.ownerAddress) {
            setError(error,"stable infrastructure object changed ownerAddress inside one journal stream");
            return false;
        }
        latest[key]=record;
    }

    out.reserve(latest.size());
    for (auto& [key,record]:latest) {
        (void)key;
        if (record.op==InfrastructureJournalOp::Tombstone &&
            policy==InfrastructureCompactionPolicy::DropTombstonesAgainstEmptyBaseline) continue;
        out.push_back(std::move(record));
    }
    std::sort(out.begin(),out.end(),deterministicRecordLess);
    return true;
}

bool validateInfrastructureSnapshotClosure(const std::vector<InfrastructureJournalRecord>& records,
                                            std::string* error) {
    std::vector<InfrastructureJournalRecord> compacted;
    if (!compactInfrastructureJournal(records,compacted,InfrastructureCompactionPolicy::KeepTombstones,error)) return false;

    std::map<RefKey,const InfrastructureJournalRecord*> surviving;
    std::map<std::uint64_t,RefKey> globalIds;
    for (const auto& record:compacted) {
        if (record.op!=InfrastructureJournalOp::Upsert) continue;
        const auto key=recordKey(record);
        const auto [idIt,idInserted]=globalIds.emplace(record.stableId,key);
        if (!idInserted && idIt->second!=key) {
            setError(error,"stable infrastructure ID is reused across record kinds");
            return false;
        }
        surviving[key]=&record;
    }

    for (const auto& record:compacted) {
        if (record.op!=InfrastructureJournalOp::Upsert) continue;
        const auto expected=expectedDependencyKeys(record);
        for (const auto& key:expected) {
            const auto found=surviving.find(key);
            if (found==surviving.end()) {
                setError(error,"infrastructure snapshot has a dangling/tombstoned stable dependency");
                return false;
            }
            if (record.schemaVersion>=2) {
                const auto dep=std::find_if(record.dependencies.begin(),record.dependencies.end(),[&](const auto& ref){return refKey(ref)==key;});
                if (dep==record.dependencies.end() || dep->ownerAddress!=found->second->ownerAddress) {
                    setError(error,"infrastructure dependency owner address does not match referenced record shard");
                    return false;
                }
            }
        }
    }
    return true;
}

bool shardInfrastructureRecordsByOwnerAddress(const std::vector<InfrastructureJournalRecord>& records,
                                              std::vector<InfrastructureJournalShard>& out,
                                              std::string* error) {
    out.clear();
    std::vector<InfrastructureJournalRecord> validated=records;
    for (const auto& record:validated) if (!validateInfrastructureJournalRecord(record,error)) return false;
    std::sort(validated.begin(),validated.end(),[](const auto& a,const auto& b){
        if (a.ownerAddress!=b.ownerAddress) return addressLess(a.ownerAddress,b.ownerAddress);
        return deterministicRecordLess(a,b);
    });
    for (const auto& record:validated) {
        if (out.empty() || out.back().ownerAddress!=record.ownerAddress) {
            out.push_back({record.ownerAddress,{}});
        }
        out.back().records.push_back(record);
    }
    return true;
}

bool collectMissingInfrastructureDependencies(const SurfaceInfrastructure& infrastructure,
                                               const std::vector<InfrastructureJournalRecord>& records,
                                               std::vector<InfrastructureStableRef>& out,
                                               std::string* error) {
    out.clear();
    std::vector<InfrastructureJournalRecord> compacted;
    if (!compactInfrastructureJournal(records,compacted,InfrastructureCompactionPolicy::KeepTombstones,error)) return false;

    std::map<RefKey,const InfrastructureJournalRecord*> batchUpserts;
    for (const auto& record:compacted) {
        if (record.op==InfrastructureJournalOp::Upsert) batchUpserts[recordKey(record)]=&record;
    }

    auto liveMatches=[&](const InfrastructureStableRef& ref) {
        switch(ref.kind) {
            case InfrastructureRecordKind::Machine: {
                const auto* object=infrastructure.find(ref.stableId);
                return object && object->anchor==ref.ownerAddress;
            }
            case InfrastructureRecordKind::Portal: {
                const auto* object=infrastructure.findPortal(ref.stableId);
                return object && object->anchor==ref.ownerAddress;
            }
            case InfrastructureRecordKind::AirlockAssembly: {
                const auto* object=infrastructure.findAirlockAssembly(ref.stableId);
                return object && object->chamberAnchor==ref.ownerAddress;
            }
            case InfrastructureRecordKind::AutomationRule: {
                return infrastructure.findAutomationRule(ref.stableId)!=nullptr;
            }
        }
        return false;
    };

    std::map<std::tuple<int,std::uint64_t,int,int,int,int>,InfrastructureStableRef> missing;
    for (const auto& record:compacted) {
        if (record.op!=InfrastructureJournalOp::Upsert) continue;
        for (const auto& dep:record.dependencies) {
            bool satisfied=liveMatches(dep);
            if (!satisfied) {
                const auto it=batchUpserts.find(refKey(dep));
                satisfied=it!=batchUpserts.end() && it->second->ownerAddress==dep.ownerAddress;
            }
            if (satisfied) continue;
            const auto key=std::make_tuple(static_cast<int>(dep.kind),dep.stableId,static_cast<int>(dep.ownerAddress.face),
                                           dep.ownerAddress.u,dep.ownerAddress.v,dep.ownerAddress.radial);
            missing.emplace(key,dep);
        }
    }
    for (const auto& [_,ref]:missing) out.push_back(ref);
    return true;
}

bool applyInfrastructureJournal(SurfaceInfrastructure& infrastructure,
                                PlanetSurface& planet,
                                const std::vector<InfrastructureJournalRecord>& records,
                                std::string* error) {
    std::vector<const InfrastructureJournalRecord*> upserts;
    std::vector<const InfrastructureJournalRecord*> tombstones;
    upserts.reserve(records.size()); tombstones.reserve(records.size());
    for (const auto& record:records) {
        if (!validateInfrastructureJournalRecord(record,error)) return false;
        (record.op==InfrastructureJournalOp::Upsert?upserts:tombstones).push_back(&record);
    }
    std::sort(upserts.begin(),upserts.end(),[](const auto* a,const auto* b){return deterministicRecordLess(*a,*b);});
    std::sort(tombstones.begin(),tombstones.end(),[](const auto* a,const auto* b){return deterministicRecordLess(*a,*b);});

    // Apply dependency-ready upserts in stable passes. Conveyor/Sorter/Cargo
    // Loader records may now depend on other Machine records, so rank alone is
    // no longer a topological order. The batch remains sparse and bounded by
    // the loaded journal shards.
    std::vector<bool> applied(upserts.size(),false);
    std::size_t remaining=upserts.size();
    while(remaining>0) {
        bool progress=false;
        for(std::size_t i=0;i<upserts.size();++i) {
            if(applied[i] || !allDependenciesPresent(infrastructure,*upserts[i])) continue;
            if(!applyOneUpsert(infrastructure,*upserts[i],error)) return false;
            applied[i]=true;
            --remaining;
            progress=true;
        }
        if(!progress) {
            if(error) *error="infrastructure upsert dependency cycle or unresolved external shard";
            return false;
        }
    }

    for (const auto* record:tombstones) if (!applyOneTombstone(infrastructure,planet,*record,error)) return false;
    return true;
}

} // namespace elysium
