#include "world/BaseInfrastructure.hpp"

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
constexpr float kBatteryCapacity = 60.0f;
constexpr float kBatteryChargeRate = 15.0f;
constexpr float kBatteryDischargeRate = 15.0f;

float demandFor(MachineType type) {
    if (type == MachineType::AtmosphereUnit) return kAtmosphereDemand;
    if (type == MachineType::AirlockController) return 2.5f;
    if (type == MachineType::SensorMast) return 1.5f;
    if (type == MachineType::Turret) return 4.0f;
    if (type == MachineType::ShieldPylon) return 12.0f;
    if (type == MachineType::LogicController) return 1.0f;
    if (type == MachineType::AlloyCrucible) return 5.0f;
    if (type == MachineType::Refinery) return 8.0f;
    if (type == MachineType::NetworkStorage) return 1.5f;
    if (type == MachineType::Conveyor) return 0.75f;
    if (type == MachineType::Sorter) return 1.0f;
    if (type == MachineType::CargoLoader) return 2.0f;
    if (type == MachineType::Crusher) return 4.0f;
    if (type == MachineType::ChemicalVat) return 6.0f;
    if (type == MachineType::Fabricator) return 9.0f;
    if (type == MachineType::Extractor) return 10.0f;
    return 0.0f;
}

int priorityFor(MachineType type) {
    // The specification puts atmosphere/life support in Critical priority.
    if (type == MachineType::AtmosphereUnit) return 1;
    if (type == MachineType::AirlockController) return 2;
    if (type == MachineType::SensorMast || type == MachineType::Turret || type == MachineType::ShieldPylon || type == MachineType::LogicController) return 2;
    if (type == MachineType::NetworkStorage || type == MachineType::Conveyor || type == MachineType::Sorter || type == MachineType::CargoLoader) return 3;
    if (type == MachineType::AlloyCrucible || type == MachineType::Refinery || type == MachineType::Crusher ||
        type == MachineType::ChemicalVat || type == MachineType::Fabricator || type == MachineType::Extractor) return 4;
    return 5;
}

float distSq(IVec3 a, IVec3 b) {
    const float dx=static_cast<float>(a.x-b.x);
    const float dy=static_cast<float>(a.y-b.y);
    const float dz=static_cast<float>(a.z-b.z);
    return dx*dx+dy*dy+dz*dz;
}

struct Dsu {
    std::vector<int> p;
    explicit Dsu(int n):p(static_cast<std::size_t>(n)){ std::iota(p.begin(),p.end(),0); }
    int find(int x){ return p[static_cast<std::size_t>(x)]==x?x:(p[static_cast<std::size_t>(x)]=find(p[static_cast<std::size_t>(x)])); }
    void join(int a,int b){ a=find(a);b=find(b);if(a!=b)p[static_cast<std::size_t>(b)]=a; }
};

} // namespace

const char* machineName(MachineType type) {
    switch(type) {
        case MachineType::BurnerGenerator: return "Burner Generator";
        case MachineType::BatteryBank: return "Battery Bank";
        case MachineType::AtmosphereUnit: return "Atmosphere Unit";
        case MachineType::StorageCrate: return "Storage Crate";
        case MachineType::AirlockController: return "Airlock Controller";
        case MachineType::SensorMast: return "Sensor Mast";
        case MachineType::Turret: return "Turret";
        case MachineType::ShieldPylon: return "Shield Pylon";
        case MachineType::LogicController: return "Logic Controller";
        case MachineType::Furnace: return "Furnace";
        case MachineType::AlloyCrucible: return "Alloy Crucible";
        case MachineType::Refinery: return "Refinery";
        case MachineType::NetworkStorage: return "Network Storage";
        case MachineType::Conveyor: return "Conveyor";
        case MachineType::Sorter: return "Sorter";
        case MachineType::CargoLoader: return "Cargo Loader";
        case MachineType::Crusher: return "Crusher";
        case MachineType::ChemicalVat: return "Chemical Vat";
        case MachineType::Fabricator: return "Fabricator";
        case MachineType::Extractor: return "Extractor";
    }
    return "Machine";
}

BaseInfrastructure::BaseInfrastructure(std::uint64_t worldSeed):worldSeed_(worldSeed){}

std::uint64_t BaseInfrastructure::allocateStableId() {
    // Stable object IDs are save identities, never ECS entities or GPU handles.
    // Serial is persisted indirectly by restoring IDs and advancing nextSerial.
    for (;;) {
        const std::uint64_t id=mix64(worldSeed_ ^ 0x4F424A454354ULL ^ nextSerial_++);
        if (id==0) continue;
        if (!find(id)) return id;
    }
}

std::uint64_t BaseInfrastructure::place(MachineType type, IVec3 anchor) {
    MachineObject o{};
    o.stableId=allocateStableId();
    o.type=type;
    o.anchor=anchor;
    if(type==MachineType::BatteryBank) o.storedEnergy=0.0f;
    objects_.push_back(o);
    return o.stableId;
}

bool BaseInfrastructure::restore(const MachineObject& object) {
    if(object.stableId==0 || find(object.stableId)) return false;
    MachineObject copy=object;
    copy.powered=false; // derived state is rebuilt by update()
    if(copy.type==MachineType::BatteryBank) copy.storedEnergy=std::clamp(copy.storedEnergy,0.0f,kBatteryCapacity);
    copy.fuelSeconds=std::max(0.0f,copy.fuelSeconds);
    objects_.push_back(copy);
    // The hash-based ID is not directly invertible to a serial; simply ensure
    // future allocations probe until they find a free ID.
    return true;
}

bool BaseInfrastructure::remove(std::uint64_t stableId) {
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    if(it==objects_.end()) return false;
    objects_.erase(it);
    return true;
}

MachineObject* BaseInfrastructure::find(std::uint64_t stableId) {
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    return it==objects_.end()?nullptr:&*it;
}
const MachineObject* BaseInfrastructure::find(std::uint64_t stableId) const {
    const auto it=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return o.stableId==stableId;});
    return it==objects_.end()?nullptr:&*it;
}

MachineObject* BaseInfrastructure::nearest(MachineType type, Vec3 position, float maxDistance) {
    MachineObject* best=nullptr;
    float bestSq=maxDistance*maxDistance;
    for(auto& o:objects_) {
        if(o.type!=type) continue;
        const Vec3 c{static_cast<float>(o.anchor.x)+0.5f,static_cast<float>(o.anchor.y)+0.5f,static_cast<float>(o.anchor.z)+0.5f};
        const float d=lengthSq(c-position);
        if(d<=bestSq){bestSq=d;best=&o;}
    }
    return best;
}

void BaseInfrastructure::update(float dt) {
    summary_={};
    if(objects_.empty()) return;
    for(auto& o:objects_) o.powered=false;

    Dsu dsu(static_cast<int>(objects_.size()));
    const float linkSq=kLinkRadius*kLinkRadius;
    for(int i=0;i<static_cast<int>(objects_.size());++i)
        for(int j=i+1;j<static_cast<int>(objects_.size());++j)
            if(distSq(objects_[static_cast<std::size_t>(i)].anchor,objects_[static_cast<std::size_t>(j)].anchor)<=linkSq)
                dsu.join(i,j);

    std::unordered_map<int,std::vector<int>> networks;
    for(int i=0;i<static_cast<int>(objects_.size());++i) networks[dsu.find(i)].push_back(i);
    summary_.networkCount=static_cast<int>(networks.size());

    for(auto& [_,ids]:networks) {
        std::sort(ids.begin(),ids.end(),[&](int a,int b){return objects_[static_cast<std::size_t>(a)].stableId<objects_[static_cast<std::size_t>(b)].stableId;});
        float generation=0.0f;
        std::vector<int> batteries;
        std::vector<int> loads;
        for(const int id:ids) {
            auto& o=objects_[static_cast<std::size_t>(id)];
            if(!o.enabled) continue;
            if(o.type==MachineType::BurnerGenerator && o.fuelSeconds>0.0f) {
                generation += kBurnerGeneration;
                o.powered=true;
                o.fuelSeconds=std::max(0.0f,o.fuelSeconds-dt);
            } else if(o.type==MachineType::BatteryBank) batteries.push_back(id);
            else if(demandFor(o.type)>0.0f) loads.push_back(id);
            else o.powered=true; // passive storage etc.
        }

        std::sort(loads.begin(),loads.end(),[&](int a,int b){
            const auto& A=objects_[static_cast<std::size_t>(a)];
            const auto& B=objects_[static_cast<std::size_t>(b)];
            if(priorityFor(A.type)!=priorityFor(B.type)) return priorityFor(A.type)<priorityFor(B.type);
            return A.stableId<B.stableId;
        });

        float batteryAvailable=0.0f,batteryCapacity=0.0f;
        for(const int id:batteries){
            auto& b=objects_[static_cast<std::size_t>(id)];
            batteryAvailable += std::min(b.storedEnergy,kBatteryDischargeRate*dt);
            batteryCapacity += kBatteryCapacity;
        }

        float available=generation+batteryAvailable;
        float demand=0.0f,supplied=0.0f;
        for(const int id:loads) {
            auto& o=objects_[static_cast<std::size_t>(id)];
            const float d=demandFor(o.type);
            demand+=d;
            if(available+1e-4f>=d) {
                o.powered=true;
                available-=d;
                supplied+=d;
                ++summary_.poweredLoads;
            } else ++summary_.shedLoads;
        }

        const float generationUsed=std::min(generation,supplied);
        float batteryNeeded=std::max(0.0f,supplied-generationUsed);
        for(const int id:batteries) {
            auto& b=objects_[static_cast<std::size_t>(id)];
            const float take=std::min({b.storedEnergy,kBatteryDischargeRate*dt,batteryNeeded});
            b.storedEnergy-=take;
            batteryNeeded-=take;
            b.powered=true;
        }

        float surplus=std::max(0.0f,generation-supplied);
        for(const int id:batteries) {
            auto& b=objects_[static_cast<std::size_t>(id)];
            const float room=kBatteryCapacity-b.storedEnergy;
            const float put=std::min({room,kBatteryChargeRate*dt,surplus});
            b.storedEnergy+=put;
            surplus-=put;
            b.powered=true;
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

bool BaseInfrastructure::oxygenatedAt(const World& world, IVec3 cell, int maxRoomCells) const {
    if(!world.inBounds(cell.x,cell.y,cell.z) || world.isSolid(cell.x,cell.y,cell.z)) return false;
    for(const auto& o:objects_) {
        if(o.type!=MachineType::AtmosphereUnit || !o.enabled || !o.powered) continue;
        const auto room=world.sealedVolume(o.anchor,maxRoomCells);
        if(!room.sealed) continue;
        const int target=world.flatIndex(cell.x,cell.y,cell.z);
        if(std::find(room.cells.begin(),room.cells.end(),target)!=room.cells.end()) return true;
    }
    return false;
}

} // namespace elysium
