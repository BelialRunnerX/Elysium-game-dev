#include "world/SurfaceIndustry.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace elysium {
namespace {

constexpr int blockItem(BlockType type) { return static_cast<int>(type); }
constexpr int item(IndustryItemId id) { return static_cast<int>(id); }

constexpr std::array<SurfaceRecipeDefinition,27> kRecipes{{
    {SurfaceRecipeId::SmeltCopper, MachineType::Furnace, "Smelt Copper",
        {{{blockItem(BlockType::CopperOre),1},{0,0},{0,0}}},1,item(IndustryItemId::CopperIngot),1,4.0f},
    {SurfaceRecipeId::SmeltTin, MachineType::Furnace, "Smelt Tin",
        {{{blockItem(BlockType::TinOre),1},{0,0},{0,0}}},1,item(IndustryItemId::TinIngot),1,4.0f},
    {SurfaceRecipeId::SmeltIron, MachineType::Furnace, "Smelt Iron",
        {{{blockItem(BlockType::IronOre),1},{0,0},{0,0}}},1,item(IndustryItemId::IronIngot),1,5.0f},
    {SurfaceRecipeId::CharCoal, MachineType::Furnace, "Carbonize Coal",
        {{{blockItem(BlockType::CoalOre),1},{0,0},{0,0}}},1,item(IndustryItemId::Carbon),1,3.5f},
    {SurfaceRecipeId::AlloyBronze, MachineType::AlloyCrucible, "Alloy Bronze",
        {{{item(IndustryItemId::CopperIngot),3},{item(IndustryItemId::TinIngot),1},{0,0}}},2,item(IndustryItemId::BronzeIngot),4,7.0f},
    {SurfaceRecipeId::AlloySteel, MachineType::AlloyCrucible, "Alloy Steel",
        {{{item(IndustryItemId::IronIngot),1},{item(IndustryItemId::Carbon),1},{0,0}}},2,item(IndustryItemId::SteelIngot),1,7.0f},
    {SurfaceRecipeId::RefineCopper, MachineType::Refinery, "Refine Copper",
        {{{blockItem(BlockType::CopperOre),1},{0,0},{0,0}}},1,item(IndustryItemId::CopperIngot),2,6.0f},
    {SurfaceRecipeId::RefineTin, MachineType::Refinery, "Refine Tin",
        {{{blockItem(BlockType::TinOre),1},{0,0},{0,0}}},1,item(IndustryItemId::TinIngot),2,6.0f},
    {SurfaceRecipeId::RefineIron, MachineType::Refinery, "Refine Iron",
        {{{blockItem(BlockType::IronOre),1},{0,0},{0,0}}},1,item(IndustryItemId::IronIngot),2,7.0f},

    {SurfaceRecipeId::CrushStone, MachineType::Crusher, "Crush Stone",
        {{{blockItem(BlockType::Stone),1},{0,0},{0,0}}},1,item(IndustryItemId::StoneAggregate),2,2.0f},
    {SurfaceRecipeId::CrushCopper, MachineType::Crusher, "Crush Copper Ore",
        {{{blockItem(BlockType::CopperOre),1},{0,0},{0,0}}},1,item(IndustryItemId::CopperConcentrate),2,3.0f},
    {SurfaceRecipeId::CrushTin, MachineType::Crusher, "Crush Tin Ore",
        {{{blockItem(BlockType::TinOre),1},{0,0},{0,0}}},1,item(IndustryItemId::TinConcentrate),2,3.0f},
    {SurfaceRecipeId::CrushIron, MachineType::Crusher, "Crush Iron Ore",
        {{{blockItem(BlockType::IronOre),1},{0,0},{0,0}}},1,item(IndustryItemId::IronConcentrate),2,3.5f},

    {SurfaceRecipeId::RefineCopperConcentrate, MachineType::Refinery, "Refine Copper Concentrate",
        {{{item(IndustryItemId::CopperConcentrate),1},{0,0},{0,0}}},1,item(IndustryItemId::CopperIngot),1,3.5f},
    {SurfaceRecipeId::RefineTinConcentrate, MachineType::Refinery, "Refine Tin Concentrate",
        {{{item(IndustryItemId::TinConcentrate),1},{0,0},{0,0}}},1,item(IndustryItemId::TinIngot),1,3.5f},
    {SurfaceRecipeId::RefineIronConcentrate, MachineType::Refinery, "Refine Iron Concentrate",
        {{{item(IndustryItemId::IronConcentrate),1},{0,0},{0,0}}},1,item(IndustryItemId::IronIngot),1,4.0f},

    // Chemical/fabrication recipes deliberately use only materials obtainable
    // in the current vertical slice. More exact Appendix-H recipes replace
    // these prototype substitutions when silver/resin/fiber/etc. enter worldgen.
    {SurfaceRecipeId::MixSealant, MachineType::ChemicalVat, "Mix Sealant",
        {{{item(IndustryItemId::StoneAggregate),2},{item(IndustryItemId::Carbon),1},{0,0}}},2,item(IndustryItemId::Sealant),1,5.0f},
    {SurfaceRecipeId::FabricateCopperWire, MachineType::Fabricator, "Draw Copper Wire",
        {{{item(IndustryItemId::CopperIngot),1},{0,0},{0,0}}},1,item(IndustryItemId::CopperWire),4,3.0f},
    {SurfaceRecipeId::FabricateSteelFrame, MachineType::Fabricator, "Fabricate Steel Frame",
        {{{item(IndustryItemId::SteelIngot),2},{0,0},{0,0}}},1,item(IndustryItemId::SteelFrame),1,5.0f},
    {SurfaceRecipeId::FabricateActuator, MachineType::Fabricator, "Fabricate Actuator",
        {{{item(IndustryItemId::SteelIngot),1},{item(IndustryItemId::BronzeIngot),1},{0,0}}},2,item(IndustryItemId::Actuator),1,5.5f},
    {SurfaceRecipeId::FabricateControlCircuit, MachineType::Fabricator, "Fabricate Control Circuit",
        {{{item(IndustryItemId::CopperWire),2},{item(IndustryItemId::Carbon),1},{0,0}}},2,item(IndustryItemId::ControlCircuit),1,4.0f},
    {SurfaceRecipeId::FabricateSensorPackage, MachineType::Fabricator, "Fabricate Sensor Package",
        {{{item(IndustryItemId::ControlCircuit),1},{item(IndustryItemId::CopperWire),2},{item(IndustryItemId::SteelFrame),1}}},3,item(IndustryItemId::SensorPackage),1,7.0f},
    {SurfaceRecipeId::FabricateTurretAmmo, MachineType::Fabricator, "Fabricate Turret Ammunition",
        {{{item(IndustryItemId::SteelIngot),1},{item(IndustryItemId::Carbon),1},{0,0}}},2,item(IndustryItemId::TurretAmmo),12,4.5f},
    {SurfaceRecipeId::FabricateRepairKit, MachineType::Fabricator, "Fabricate Repair Kit",
        {{{item(IndustryItemId::SteelFrame),1},{item(IndustryItemId::Sealant),1},{0,0}}},2,item(IndustryItemId::RepairKit),1,6.0f},
    {SurfaceRecipeId::FabricateFilterCartridge, MachineType::Fabricator, "Fabricate Filter Cartridge",
        {{{item(IndustryItemId::Carbon),1},{blockItem(BlockType::Planks),1},{0,0}}},2,item(IndustryItemId::FilterCartridge),2,4.0f},
    {SurfaceRecipeId::FabricateMachineCasing, MachineType::Fabricator, "Fabricate Machine Casing",
        {{{item(IndustryItemId::SteelFrame),1},{item(IndustryItemId::CopperWire),2},{0,0}}},2,item(IndustryItemId::MachineCasing),1,6.0f},
    {SurfaceRecipeId::FabricateCompositePanel, MachineType::Fabricator, "Fabricate Composite Panel",
        {{{blockItem(BlockType::Planks),1},{item(IndustryItemId::Sealant),1},{item(IndustryItemId::CopperWire),1}}},3,item(IndustryItemId::CompositePanel),2,5.0f},
}};


bool validItemId(int itemId) {
    if(itemId > 0 && itemId < kBlockTypeCount) return true;
    return validIndustryItemId(itemId);
}

} // namespace

const char* industryItemName(int itemId) {
    if(itemId > 0 && itemId < kBlockTypeCount)
        return blockProperties(static_cast<BlockType>(itemId)).name.data();
    switch(static_cast<IndustryItemId>(itemId)) {
        case IndustryItemId::CopperIngot: return "Copper Ingot";
        case IndustryItemId::TinIngot: return "Tin Ingot";
        case IndustryItemId::IronIngot: return "Iron Ingot";
        case IndustryItemId::Carbon: return "Carbon";
        case IndustryItemId::BronzeIngot: return "Bronze Ingot";
        case IndustryItemId::SteelIngot: return "Steel Ingot";
        case IndustryItemId::StoneAggregate: return "Stone Aggregate";
        case IndustryItemId::CopperConcentrate: return "Copper Concentrate";
        case IndustryItemId::TinConcentrate: return "Tin Concentrate";
        case IndustryItemId::IronConcentrate: return "Iron Concentrate";
        case IndustryItemId::Sealant: return "Sealant";
        case IndustryItemId::CopperWire: return "Copper Wire";
        case IndustryItemId::SteelFrame: return "Steel Frame";
        case IndustryItemId::Actuator: return "Actuator";
        case IndustryItemId::ControlCircuit: return "Control Circuit";
        case IndustryItemId::SensorPackage: return "Sensor Package";
        case IndustryItemId::TurretAmmo: return "Turret Ammunition";
        case IndustryItemId::RepairKit: return "Repair Kit";
        case IndustryItemId::FilterCartridge: return "Filter Cartridge";
        case IndustryItemId::MachineCasing: return "Machine Casing";
        case IndustryItemId::CompositePanel: return "Composite Panel";
    }
    return "Unknown Item";
}

bool validIndustryItemId(int itemId) {
    return itemId >= static_cast<int>(IndustryItemId::CopperIngot) &&
           itemId <= static_cast<int>(IndustryItemId::CompositePanel);
}

const SurfaceRecipeDefinition* surfaceRecipe(SurfaceRecipeId id) {
    const auto it=std::find_if(kRecipes.begin(),kRecipes.end(),[&](const auto& r){return r.id==id;});
    return it==kRecipes.end()?nullptr:&*it;
}

std::vector<const SurfaceRecipeDefinition*> surfaceRecipesForMachine(MachineType machine) {
    std::vector<const SurfaceRecipeDefinition*> out;
    for(const auto& recipe:kRecipes) if(recipe.machine==machine) out.push_back(&recipe);
    return out;
}

bool SurfaceIndustrySystem::processMachine(MachineType type) {
    return type==MachineType::Furnace || type==MachineType::AlloyCrucible || type==MachineType::Refinery ||
           type==MachineType::Crusher || type==MachineType::ChemicalVat || type==MachineType::Fabricator;
}

bool SurfaceIndustrySystem::poweredProcessMachine(MachineType type) {
    return type==MachineType::AlloyCrucible || type==MachineType::Refinery || type==MachineType::Crusher ||
           type==MachineType::ChemicalVat || type==MachineType::Fabricator;
}

bool SurfaceIndustrySystem::furnaceMachine(MachineType type) {
    return type==MachineType::Furnace;
}

bool SurfaceIndustrySystem::logisticsMachine(MachineType type) {
    return type==MachineType::Conveyor || type==MachineType::Sorter || type==MachineType::CargoLoader;
}

float SurfaceIndustrySystem::logisticsInterval(MachineType type) {
    switch(type) {
        case MachineType::Conveyor: return ConveyorTransferSeconds;
        case MachineType::Sorter: return SorterTransferSeconds;
        case MachineType::CargoLoader: return CargoLoaderTransferSeconds;
        default: return std::numeric_limits<float>::infinity();
    }
}

int SurfaceIndustrySystem::logisticsBatch(MachineType type) {
    return type==MachineType::CargoLoader ? CargoLoaderBatch : 1;
}

int SurfaceIndustrySystem::inventoryCapacity(const SurfaceMachineObject& machine) {
    if(machine.type==MachineType::NetworkStorage) return MaxNetworkStorageStacks;
    if(machine.type==MachineType::StorageCrate) return MaxStorageCrateStacks;
    if(processMachine(machine.type) || machine.type==MachineType::Extractor || machine.type==MachineType::Conveyor ||
       machine.type==MachineType::Sorter || machine.type==MachineType::CargoLoader)
        return MaxProcessStacks;
    return 4;
}

bool SurfaceIndustrySystem::normalizeInventory(SurfaceMachineObject& machine) {
    for(auto& stack:machine.inventory) {
        stack.count=std::clamp(stack.count,0,MaxStackCount);
    }
    machine.inventory.erase(std::remove_if(machine.inventory.begin(),machine.inventory.end(),[](const auto& s){
        return !validItemId(s.itemId) || s.count<=0;
    }),machine.inventory.end());
    std::sort(machine.inventory.begin(),machine.inventory.end(),[](const auto& a,const auto& b){return a.itemId<b.itemId;});
    std::vector<SurfaceItemStack> merged;
    merged.reserve(machine.inventory.size());
    for(const auto& stack:machine.inventory) {
        if(!merged.empty() && merged.back().itemId==stack.itemId)
            merged.back().count=std::min(MaxStackCount,merged.back().count+stack.count);
        else
            merged.push_back(stack);
    }
    if(static_cast<int>(merged.size())>inventoryCapacity(machine)) merged.resize(static_cast<std::size_t>(inventoryCapacity(machine)));
    const bool changed=merged!=machine.inventory;
    machine.inventory=std::move(merged);
    return changed;
}

int SurfaceIndustrySystem::localCount(const SurfaceMachineObject& machine,int itemId) {
    const auto it=std::lower_bound(machine.inventory.begin(),machine.inventory.end(),itemId,[](const auto& stack,int id){return stack.itemId<id;});
    return it!=machine.inventory.end() && it->itemId==itemId ? it->count : 0;
}

bool SurfaceIndustrySystem::localCanInsert(const SurfaceMachineObject& machine,int itemId,int count) {
    if(count<=0 || !validItemId(itemId)) return false;
    const auto it=std::lower_bound(machine.inventory.begin(),machine.inventory.end(),itemId,[](const auto& stack,int id){return stack.itemId<id;});
    if(it!=machine.inventory.end() && it->itemId==itemId) return it->count+count<=MaxStackCount;
    return static_cast<int>(machine.inventory.size())<inventoryCapacity(machine) && count<=MaxStackCount;
}

bool SurfaceIndustrySystem::logisticsCanExtractItem(const SurfaceMachineObject& machine,int itemId) {
    if(machine.type==MachineType::StorageCrate || machine.type==MachineType::NetworkStorage ||
       machine.type==MachineType::Conveyor || machine.type==MachineType::Sorter ||
       machine.type==MachineType::CargoLoader || machine.type==MachineType::Extractor) return validItemId(itemId);
    if(processMachine(machine.type)) {
        for(const auto* recipe:surfaceRecipesForMachine(machine.type))
            if(recipe->outputItemId==itemId) return true;
    }
    return false;
}

bool SurfaceIndustrySystem::logisticsCanReceiveItem(const SurfaceMachineObject& machine,int itemId) {
    if(machine.type==MachineType::StorageCrate || machine.type==MachineType::NetworkStorage ||
       machine.type==MachineType::Conveyor || machine.type==MachineType::Sorter ||
       machine.type==MachineType::CargoLoader) return validItemId(itemId);
    if(processMachine(machine.type)) {
        for(const auto* recipe:surfaceRecipesForMachine(machine.type)) {
            for(int i=0;i<recipe->inputCount;++i)
                if(recipe->inputs[static_cast<std::size_t>(i)].itemId==itemId) return true;
        }
    }
    return false;
}

int SurfaceIndustrySystem::localInsert(SurfaceMachineObject& machine,int itemId,int count) {
    if(count<=0 || !validItemId(itemId)) return 0;
    normalizeInventory(machine);
    auto it=std::lower_bound(machine.inventory.begin(),machine.inventory.end(),itemId,[](const auto& stack,int id){return stack.itemId<id;});
    if(it!=machine.inventory.end() && it->itemId==itemId) {
        const int accepted=std::min(count,MaxStackCount-it->count);
        it->count+=accepted;
        return accepted;
    }
    if(static_cast<int>(machine.inventory.size())>=inventoryCapacity(machine)) return 0;
    const int accepted=std::min(count,MaxStackCount);
    machine.inventory.insert(it,{itemId,accepted});
    return accepted;
}

int SurfaceIndustrySystem::localExtract(SurfaceMachineObject& machine,int itemId,int count) {
    if(count<=0) return 0;
    normalizeInventory(machine);
    auto it=std::lower_bound(machine.inventory.begin(),machine.inventory.end(),itemId,[](const auto& stack,int id){return stack.itemId<id;});
    if(it==machine.inventory.end() || it->itemId!=itemId) return 0;
    const int taken=std::min(count,it->count);
    it->count-=taken;
    if(it->count<=0) machine.inventory.erase(it);
    return taken;
}

bool SurfaceIndustrySystem::insert(SurfaceInfrastructure& infrastructure,std::uint64_t machineStableId,int itemId,int count) {
    auto* machine=infrastructure.find(machineStableId);
    if(!machine) return false;
    return localInsert(*machine,itemId,count)==count;
}

int SurfaceIndustrySystem::extract(SurfaceInfrastructure& infrastructure,std::uint64_t machineStableId,int itemId,int count) {
    auto* machine=infrastructure.find(machineStableId);
    return machine?localExtract(*machine,itemId,count):0;
}

int SurfaceIndustrySystem::itemCount(const SurfaceInfrastructure& infrastructure,std::uint64_t machineStableId,int itemId) const {
    const auto* machine=infrastructure.find(machineStableId);
    return machine?localCount(*machine,itemId):0;
}

bool SurfaceIndustrySystem::selectRecipe(SurfaceInfrastructure& infrastructure,std::uint64_t machineStableId,SurfaceRecipeId recipeId) {
    auto* machine=infrastructure.find(machineStableId);
    const auto* recipe=surfaceRecipe(recipeId);
    if(!machine || !recipe || recipe->machine!=machine->type || machine->activeRecipeId!=0) return false;
    machine->selectedRecipeId=static_cast<int>(recipeId);
    return true;
}

bool SurfaceIndustrySystem::configureLogisticsLink(SurfaceInfrastructure& infrastructure,
                                                    const PlanetSurface& planet,
                                                    std::uint64_t transportStableId,
                                                    std::uint64_t sourceStableId,
                                                    std::uint64_t targetStableId,
                                                    std::uint64_t alternateTargetStableId,
                                                    int sorterFilterItemId) {
    auto* transport=infrastructure.find(transportStableId);
    auto* source=infrastructure.find(sourceStableId);
    auto* target=infrastructure.find(targetStableId);
    if(!transport || !logisticsMachine(transport->type) || !source || !target) return false;
    if(transportStableId==sourceStableId || transportStableId==targetStableId || sourceStableId==targetStableId) return false;

    SurfaceMachineObject* alternate=nullptr;
    if(transport->type==MachineType::Sorter) {
        if(alternateTargetStableId==0 || sorterFilterItemId<=0) return false;
        alternate=infrastructure.find(alternateTargetStableId);
        if(!alternate || alternateTargetStableId==transportStableId || alternateTargetStableId==sourceStableId || alternateTargetStableId==targetStableId) return false;
    } else if(alternateTargetStableId!=0 || sorterFilterItemId!=0) return false;

    const Vec3 center=planet.cellCenterPosition(transport->anchor);
    const float maxSq=MaxLogisticsLinkDistance*MaxLogisticsLinkDistance;
    if(lengthSq(planet.cellCenterPosition(source->anchor)-center)>maxSq ||
       lengthSq(planet.cellCenterPosition(target->anchor)-center)>maxSq ||
       (alternate && lengthSq(planet.cellCenterPosition(alternate->anchor)-center)>maxSq)) return false;

    transport->logisticsSourceStableId=sourceStableId;
    transport->logisticsTargetStableId=targetStableId;
    transport->logisticsAlternateTargetStableId=alternateTargetStableId;
    transport->sorterFilterItemId=transport->type==MachineType::Sorter ? sorterFilterItemId : 0;
    transport->logisticsProgressSeconds=0.0f;
    transport->logisticsBlocked=false;
    return true;
}

bool SurfaceIndustrySystem::clearLogisticsLink(SurfaceInfrastructure& infrastructure,std::uint64_t transportStableId) {
    auto* transport=infrastructure.find(transportStableId);
    if(!transport || !logisticsMachine(transport->type)) return false;
    transport->logisticsSourceStableId=0;
    transport->logisticsTargetStableId=0;
    transport->logisticsAlternateTargetStableId=0;
    transport->sorterFilterItemId=0;
    transport->logisticsProgressSeconds=0.0f;
    transport->logisticsBlocked=false;
    return true;
}

std::vector<SurfaceMachineObject*> SurfaceIndustrySystem::poweredNetworkStorages(SurfaceInfrastructure& infrastructure,
                                                                                 std::uint64_t networkId,
                                                                                 std::uint64_t excludeStableId) {
    std::vector<SurfaceMachineObject*> out;
    if(networkId==0) return out;
    for(auto& object:infrastructure.objects_) {
        if(object.stableId==excludeStableId || object.type!=MachineType::NetworkStorage || !object.enabled || !object.powered || object.powerNetworkId!=networkId) continue;
        out.push_back(&object);
    }
    std::sort(out.begin(),out.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    return out;
}

SurfaceMachineObject* SurfaceIndustrySystem::firstPoweredNetworkStorage(SurfaceInfrastructure& infrastructure,
                                                                         std::uint64_t networkId,
                                                                         std::uint64_t excludeStableId) {
    auto storages=poweredNetworkStorages(infrastructure,networkId,excludeStableId);
    return storages.empty()?nullptr:storages.front();
}

int SurfaceIndustrySystem::availableInput(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine,int itemId) const {
    int total=localCount(machine,itemId);
    if(machine.powerNetworkId==0) return total;
    for(const auto* storage:poweredNetworkStorages(infrastructure,machine.powerNetworkId,machine.stableId)) {
        total+=localCount(*storage,itemId);
        if(total>=MaxStackCount*4) break;
    }
    return total;
}

bool SurfaceIndustrySystem::consumeInput(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine,int itemId,int count) {
    if(count<=0) return true;
    if(availableInput(infrastructure,machine,itemId)<count) return false;
    int remaining=count;
    remaining-=localExtract(machine,itemId,remaining);
    for(auto* storage:poweredNetworkStorages(infrastructure,machine.powerNetworkId,machine.stableId)) {
        if(remaining<=0) break;
        const int before=remaining;
        remaining-=localExtract(*storage,itemId,remaining);
        if(before!=remaining) ++telemetry_.networkPulls;
    }
    telemetry_.itemsConsumed+=count-remaining;
    return remaining==0;
}

bool SurfaceIndustrySystem::canRouteOutput(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine,int itemId,int count) const {
    if(localCanInsert(machine,itemId,count)) return true;
    if(machine.powerNetworkId==0) return false;
    for(const auto* storage:poweredNetworkStorages(infrastructure,machine.powerNetworkId,machine.stableId))
        if(localCanInsert(*storage,itemId,count)) return true;
    return false;
}

bool SurfaceIndustrySystem::routeOutput(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine,int itemId,int count) {
    if(count<=0) return true;
    // Prefer powered shared storage when available. This makes Network Storage
    // visibly useful while retaining local fallback for isolated machinery.
    for(auto* storage:poweredNetworkStorages(infrastructure,machine.powerNetworkId,machine.stableId)) {
        if(localCanInsert(*storage,itemId,count)) {
            if(localInsert(*storage,itemId,count)==count) {
                ++telemetry_.networkPushes;
                telemetry_.itemsProduced+=count;
                return true;
            }
        }
    }
    if(localInsert(machine,itemId,count)==count) {
        telemetry_.itemsProduced+=count;
        return true;
    }
    return false;
}

bool SurfaceIndustrySystem::ensureFurnaceFuel(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine) {
    if(machine.fuelSeconds>0.0f) return true;
    if(!consumeInput(infrastructure,machine,blockItem(BlockType::CoalOre),1)) return false;
    machine.fuelSeconds+=FurnaceCoalSeconds;
    ++telemetry_.furnaceFuelLoads;
    return true;
}

const SurfaceRecipeDefinition* SurfaceIndustrySystem::chooseRecipe(SurfaceInfrastructure& infrastructure,SurfaceMachineObject& machine) const {
    if(machine.selectedRecipeId!=0) {
        const auto* chosen=surfaceRecipe(static_cast<SurfaceRecipeId>(machine.selectedRecipeId));
        if(chosen && chosen->machine==machine.type) return chosen;
    }
    // Auto mode picks the lowest stable recipe ID whose full input set exists.
    for(const auto* recipe:surfaceRecipesForMachine(machine.type)) {
        bool ready=true;
        for(int i=0;i<recipe->inputCount;++i)
            if(availableInput(infrastructure,machine,recipe->inputs[static_cast<std::size_t>(i)].itemId)<recipe->inputs[static_cast<std::size_t>(i)].count) {ready=false;break;}
        if(ready) return recipe;
    }
    return nullptr;
}


bool SurfaceIndustrySystem::attemptLogisticsTransfer(SurfaceInfrastructure& infrastructure,
                                                     SurfaceMachineObject& transport) {
    auto* source=infrastructure.find(transport.logisticsSourceStableId);
    auto* primary=infrastructure.find(transport.logisticsTargetStableId);
    auto* alternate=transport.logisticsAlternateTargetStableId?infrastructure.find(transport.logisticsAlternateTargetStableId):nullptr;
    if(!source || !primary || (transport.type==MachineType::Sorter && !alternate)) {
        ++telemetry_.logisticsInvalidLinks;
        transport.logisticsBlocked=true;
        return false;
    }

    normalizeInventory(*source);
    normalizeInventory(*primary);
    if(alternate) normalizeInventory(*alternate);
    if(source->inventory.empty()) {
        transport.logisticsBlocked=false;
        return false;
    }

    for(const auto& stack:source->inventory) {
        // Copy the identity before extraction: localExtract may erase the
        // source vector element and invalidate the range-for reference.
        const int itemId=stack.itemId;
        const int available=stack.count;
        if(!logisticsCanExtractItem(*source,itemId)) continue;
        SurfaceMachineObject* target=primary;
        if(transport.type==MachineType::Sorter && itemId!=transport.sorterFilterItemId) target=alternate;
        if(!target || !logisticsCanReceiveItem(*target,itemId)) continue;
        const int requested=std::min(logisticsBatch(transport.type),available);
        if(requested<=0 || !localCanInsert(*target,itemId,requested)) continue;

        const int taken=localExtract(*source,itemId,requested);
        if(taken<=0) continue;
        const int accepted=localInsert(*target,itemId,taken);
        if(accepted!=taken) {
            // Single-owner-thread update makes this unreachable after the
            // capacity preflight, but preserve item conservation if future
            // systems introduce a mutation between the two operations.
            if(accepted>0) localExtract(*target,itemId,accepted);
            localInsert(*source,itemId,taken);
            transport.logisticsBlocked=true;
            ++telemetry_.logisticsJams;
            return false;
        }

        transport.logisticsBlocked=false;
        ++telemetry_.logisticsTransfers;
        telemetry_.logisticsItemsMoved+=taken;
        if(transport.type==MachineType::Conveyor) ++telemetry_.conveyorTransfers;
        else if(transport.type==MachineType::Sorter) ++telemetry_.sorterTransfers;
        else if(transport.type==MachineType::CargoLoader) ++telemetry_.cargoTransfers;
        return true;
    }

    // Inventory exists but no currently valid destination can accept it. That
    // is a logistics jam, not an input stall and never deletes the item.
    transport.logisticsBlocked=true;
    ++telemetry_.logisticsJams;
    return false;
}

void SurfaceIndustrySystem::updateLogistics(SurfaceInfrastructure& infrastructure,float dt) {
    std::vector<SurfaceMachineObject*> transports;
    for(auto& object:infrastructure.objects_) {
        if(logisticsMachine(object.type)) {
            normalizeInventory(object);
            object.logisticsBlocked=false;
            transports.push_back(&object);
        }
    }
    std::sort(transports.begin(),transports.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    telemetry_.logisticsMachines=static_cast<int>(transports.size());

    for(auto* transport:transports) {
        if(!transport->enabled) continue;
        if(transport->logisticsSourceStableId==0 || transport->logisticsTargetStableId==0 ||
           (transport->type==MachineType::Sorter && transport->logisticsAlternateTargetStableId==0)) {
            ++telemetry_.logisticsInvalidLinks;
            transport->logisticsBlocked=true;
            continue;
        }
        if(!transport->powered) {
            ++telemetry_.logisticsStalledForPower;
            continue;
        }

        const float interval=logisticsInterval(transport->type);
        transport->logisticsProgressSeconds=std::max(0.0f,transport->logisticsProgressSeconds)+dt;
        int attempts=0;
        while(transport->logisticsProgressSeconds+1e-6f>=interval && attempts<MaxLogisticsTransfersPerUpdate) {
            ++attempts;
            const bool moved=attemptLogisticsTransfer(infrastructure,*transport);
            if(!moved) {
                transport->logisticsProgressSeconds=std::min(transport->logisticsProgressSeconds,interval);
                break;
            }
            transport->logisticsProgressSeconds-=interval;
        }
        if(attempts==MaxLogisticsTransfersPerUpdate && transport->logisticsProgressSeconds>interval)
            transport->logisticsProgressSeconds=interval;
    }
}


void SurfaceIndustrySystem::updateDefenseSupply(SurfaceInfrastructure& infrastructure) {
    std::vector<SurfaceMachineObject*> turrets;
    for(auto& object:infrastructure.objects_) if(object.type==MachineType::Turret) turrets.push_back(&object);
    std::sort(turrets.begin(),turrets.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    constexpr int kRoundsPerPack=12;
    constexpr int kTurretAmmoCapacity=96;
    for(auto* turret:turrets) {
        if(!turret->enabled || turret->powerNetworkId==0 || turret->ammo>kTurretAmmoCapacity-kRoundsPerPack) continue;
        for(auto* storage:poweredNetworkStorages(infrastructure,turret->powerNetworkId,turret->stableId)) {
            if(localExtract(*storage,item(IndustryItemId::TurretAmmo),1)!=1) continue;
            turret->ammo+=kRoundsPerPack;
            ++telemetry_.turretAmmoLoads;
            telemetry_.turretRoundsLoaded+=kRoundsPerPack;
            break;
        }
    }
}

bool SurfaceIndustrySystem::findExtractorTarget(const PlanetSurface& planet,
                                                const SurfaceMachineObject& extractor,
                                                SurfaceCellAddress& out) const {
    std::vector<SurfaceCellAddress> candidates;
    candidates.reserve(static_cast<std::size_t>((ExtractorHorizontalRadius*2+1)*(ExtractorHorizontalRadius*2+1)*ExtractorDepth));

    for(int dv=-ExtractorHorizontalRadius;dv<=ExtractorHorizontalRadius;++dv) {
        for(int du=-ExtractorHorizontalRadius;du<=ExtractorHorizontalRadius;++du) {
            if(du*du+dv*dv>ExtractorHorizontalRadius*ExtractorHorizontalRadius) continue;
            SurfaceCellAddress column=planet.normalize({extractor.anchor.face,extractor.anchor.u+du,extractor.anchor.v+dv,extractor.anchor.radial});
            const int surface=planet.surfaceRadial(column.face,column.u,column.v);
            const int bottom=std::max(0,surface-ExtractorDepth+1);
            for(int radial=surface;radial>=bottom;--radial) {
                SurfaceCellAddress address{column.face,column.u,column.v,radial};
                const BlockType type=planet.get(address);
                if(!blockProperties(type).ore || planet.playerPlaced(address)) continue;
                candidates.push_back(address);
            }
        }
    }
    if(candidates.empty()) return false;

    auto less=[](const SurfaceCellAddress& a,const SurfaceCellAddress& b){
        if(a.face!=b.face) return static_cast<int>(a.face)<static_cast<int>(b.face);
        if(a.u!=b.u) return a.u<b.u;
        if(a.v!=b.v) return a.v<b.v;
        return a.radial<b.radial;
    };
    std::sort(candidates.begin(),candidates.end(),less);
    candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
    out=candidates.front();
    return true;
}

void SurfaceIndustrySystem::updateExtraction(PlanetSurface& planet,
                                             SurfaceInfrastructure& infrastructure,
                                             float dt) {
    dt=std::max(0.0f,dt);
    std::vector<SurfaceMachineObject*> extractors;
    for(auto& object:infrastructure.objects_) {
        if(object.type==MachineType::Extractor) {
            normalizeInventory(object);
            extractors.push_back(&object);
        }
    }
    std::sort(extractors.begin(),extractors.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    telemetry_.extractorMachines=static_cast<int>(extractors.size());

    for(auto* extractor:extractors) {
        if(!extractor->enabled || !extractor->powered) continue;
        extractor->extractorProgressSeconds=std::max(0.0f,extractor->extractorProgressSeconds)+dt;
        int cycles=0;
        while(extractor->extractorProgressSeconds+1e-6f>=ExtractorCycleSeconds && cycles<MaxExtractorCyclesPerUpdate) {
            ++cycles;
            SurfaceCellAddress target{};
            if(!findExtractorTarget(planet,*extractor,target)) {
                ++telemetry_.extractorNoOre;
                extractor->extractorProgressSeconds=std::min(extractor->extractorProgressSeconds,ExtractorCycleSeconds);
                break;
            }
            const BlockType ore=planet.get(target);
            const int outputItem=blockItem(miningDrop(ore));
            if(!canRouteOutput(infrastructure,*extractor,outputItem,1)) {
                ++telemetry_.extractorOutputStalls;
                extractor->extractorProgressSeconds=std::min(extractor->extractorProgressSeconds,ExtractorCycleSeconds);
                break;
            }

            // World edit is committed only after output capacity preflight. If
            // output publication unexpectedly fails, restore the exact original
            // voxel so automated extraction can never delete matter silently.
            planet.set(target,BlockType::Air,false);
            if(!routeOutput(infrastructure,*extractor,outputItem,1)) {
                planet.set(target,ore,false);
                ++telemetry_.extractorOutputStalls;
                extractor->extractorProgressSeconds=std::min(extractor->extractorProgressSeconds,ExtractorCycleSeconds);
                break;
            }

            extractor->extractorProgressSeconds-=ExtractorCycleSeconds;
            ++telemetry_.extractorCycles;
            ++telemetry_.extractorOreMined;
            telemetry_.extractorSuspicionGenerated+=ExtractorSuspicionPerOre;
        }
        if(cycles==MaxExtractorCyclesPerUpdate && extractor->extractorProgressSeconds>ExtractorCycleSeconds)
            extractor->extractorProgressSeconds=ExtractorCycleSeconds;
    }
}

void SurfaceIndustrySystem::update(SurfaceInfrastructure& infrastructure,float dt) {
    telemetry_={};
    dt=std::max(0.0f,dt);

    // Explicit transport resolves before recipe reservation. Production output
    // therefore enters belts on the following update, which keeps transfer and
    // processing order deterministic and avoids same-tick feedback loops.
    updateLogistics(infrastructure,dt);
    updateDefenseSupply(infrastructure);

    std::vector<SurfaceMachineObject*> machines;
    machines.reserve(infrastructure.objects_.size());
    for(auto& object:infrastructure.objects_) {
        normalizeInventory(object);
        if(processMachine(object.type)) machines.push_back(&object);
    }
    std::sort(machines.begin(),machines.end(),[](const auto* a,const auto* b){return a->stableId<b->stableId;});
    telemetry_.processMachines=static_cast<int>(machines.size());

    for(auto* machine:machines) {
        if(!machine->enabled) continue;

        if(machine->activeRecipeId==0) {
            const auto* recipe=chooseRecipe(infrastructure,*machine);
            if(!recipe) {++telemetry_.stalledForInput;continue;}
            if(!canRouteOutput(infrastructure,*machine,recipe->outputItemId,recipe->outputCount)) {++telemetry_.stalledForOutput;continue;}
            bool allInputs=true;
            for(int i=0;i<recipe->inputCount;++i) {
                const auto ingredient=recipe->inputs[static_cast<std::size_t>(i)];
                if(availableInput(infrastructure,*machine,ingredient.itemId)<ingredient.count) {allInputs=false;break;}
            }
            // A cold furnace needs one additional Coal Ore beyond any coal
            // consumed by the recipe itself. This preflight prevents the fuel
            // loader from consuming the only coal and invalidating a CharCoal
            // batch before its inputs are reserved.
            if(allInputs && furnaceMachine(machine->type) && machine->fuelSeconds<=0.0f) {
                int recipeCoal=0;
                for(int i=0;i<recipe->inputCount;++i) {
                    const auto ingredient=recipe->inputs[static_cast<std::size_t>(i)];
                    if(ingredient.itemId==blockItem(BlockType::CoalOre)) recipeCoal+=ingredient.count;
                }
                if(availableInput(infrastructure,*machine,blockItem(BlockType::CoalOre))<recipeCoal+1) allInputs=false;
            }
            if(!allInputs) {++telemetry_.stalledForInput;continue;}

            // Powered machines must be energized before reserving material.
            if(poweredProcessMachine(machine->type) && !machine->powered) {++telemetry_.stalledForPower;continue;}
            if(furnaceMachine(machine->type) && !ensureFurnaceFuel(infrastructure,*machine)) {++telemetry_.stalledForInput;continue;}

            for(int i=0;i<recipe->inputCount;++i) {
                const auto ingredient=recipe->inputs[static_cast<std::size_t>(i)];
                if(!consumeInput(infrastructure,*machine,ingredient.itemId,ingredient.count)) {
                    // Preflight above makes this unreachable without concurrent mutation.
                    allInputs=false;break;
                }
            }
            if(!allInputs) {machine->activeRecipeId=0;machine->processProgressSeconds=0.0f;continue;}
            machine->activeRecipeId=static_cast<int>(recipe->id);
            machine->processProgressSeconds=0.0f;
        }

        const auto* active=surfaceRecipe(static_cast<SurfaceRecipeId>(machine->activeRecipeId));
        if(!active || active->machine!=machine->type) {
            machine->activeRecipeId=0;
            machine->processProgressSeconds=0.0f;
            continue;
        }
        if(poweredProcessMachine(machine->type) && !machine->powered) {++telemetry_.stalledForPower;continue;}
        if(furnaceMachine(machine->type) && !ensureFurnaceFuel(infrastructure,*machine)) {++telemetry_.stalledForInput;continue;}

        ++telemetry_.activeProcesses;
        float advance=dt;
        if(furnaceMachine(machine->type)) {
            advance=std::min(advance,machine->fuelSeconds);
            machine->fuelSeconds=std::max(0.0f,machine->fuelSeconds-advance);
        }
        machine->processProgressSeconds+=advance;
        if(machine->processProgressSeconds+1e-5f<active->processSeconds) continue;

        if(!canRouteOutput(infrastructure,*machine,active->outputItemId,active->outputCount)) {
            machine->processProgressSeconds=active->processSeconds;
            ++telemetry_.stalledForOutput;
            continue;
        }
        if(!routeOutput(infrastructure,*machine,active->outputItemId,active->outputCount)) {
            machine->processProgressSeconds=active->processSeconds;
            ++telemetry_.stalledForOutput;
            continue;
        }
        machine->activeRecipeId=0;
        machine->processProgressSeconds=0.0f;
        ++telemetry_.completedProcesses;
    }
}

} // namespace elysium
