#include "world/SurfaceChunkPersistence.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace elysium {
namespace {

std::uint64_t checksum64(std::string_view bytes) {
    std::uint64_t h=1469598103934665603ULL;
    for (const unsigned char c : bytes) {
        h^=static_cast<std::uint64_t>(c);
        h*=1099511628211ULL;
    }
    return h;
}


CheckedGenerationReadResult readCheckedPath(const std::filesystem::path& path,bool allowPlainLegacy) {
    CheckedGenerationReadResult result{};
    std::ifstream in(path,std::ios::binary);
    if(!in){result.error="generation file unavailable";return result;}
    const std::string bytes((std::istreambuf_iterator<char>(in)),{});
    constexpr std::string_view magic="ELYSIUM_ATOMIC_GENERATION 1\n";
    if(!bytes.starts_with(magic)) {
        if(allowPlainLegacy) {result.loaded=true;result.data=bytes;return result;}
        result.error="missing checked-generation header";return result;
    }
    std::istringstream header(bytes);
    std::string tag; int version{};
    if(!(header>>tag>>version) || tag!="ELYSIUM_ATOMIC_GENERATION" || version!=1) {result.error="invalid checked-generation header";return result;}
    std::size_t payloadBytes{}; std::uint64_t expected{};
    if(!(header>>tag>>payloadBytes) || tag!="payload_bytes") {result.error="invalid checked payload size";return result;}
    if(!(header>>tag>>expected) || tag!="checksum") {result.error="invalid checked checksum header";return result;}
    // Locate the blank-line delimiter in the original bytes rather than relying
    // on formatted stream position after operator>> whitespace consumption.
    const auto split=bytes.find("\n\n");
    if(split==std::string::npos) {result.error="missing checked-generation delimiter";return result;}
    const std::size_t start=split+2;
    if(start+payloadBytes!=bytes.size()) {result.error="checked-generation payload length mismatch";return result;}
    std::string payload=bytes.substr(start,payloadBytes);
    if(checksum64(payload)!=expected) {result.error="checked-generation checksum mismatch";return result;}
    result.loaded=true;result.data=std::move(payload);return result;
}

bool durableFlushFile(const std::filesystem::path& path) {
#if defined(_WIN32)
    const int fd=_open(path.string().c_str(),_O_RDONLY|_O_BINARY);
    if(fd<0) return false;
    const int ok=_commit(fd);
    _close(fd);
    return ok==0;
#else
    const int fd=::open(path.c_str(),O_RDONLY);
    if(fd<0) return false;
    const int ok=::fsync(fd);
    ::close(fd);
    return ok==0;
#endif
}

bool writeAllDurable(const std::filesystem::path& path,std::string_view bytes,std::string* error) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) {
        if(error) *error="could not open temporary chunk generation for writing";
        return false;
    }
    out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if(!out.good()) {
        if(error) *error="failed while writing chunk generation";
        return false;
    }
    out.close();
    if(!durableFlushFile(path)) {
        if(error) *error="chunk generation written but durable flush failed";
        return false;
    }
    return true;
}

SurfaceCellAddress decodeFlat(int index) {
    const int cellsPerFace=PlanetSurface::FaceResolution*PlanetSurface::FaceResolution*PlanetSurface::RadialLayers;
    const int f=index/cellsPerFace;
    int rem=index-f*cellsPerFace;
    const int v=rem/(PlanetSurface::FaceResolution*PlanetSurface::RadialLayers);
    rem-=v*PlanetSurface::FaceResolution*PlanetSurface::RadialLayers;
    const int u=rem/PlanetSurface::RadialLayers;
    const int r=rem-u*PlanetSurface::RadialLayers;
    return {static_cast<CubeFace>(f),u,v,r};
}

bool validChunkAddress(const PlanetChunkAddress& a) {
    return static_cast<int>(a.face)<PlanetSurface::FaceCount &&
           a.u>=0 && a.u<PlanetSurface::ChunksPerFaceAxis &&
           a.v>=0 && a.v<PlanetSurface::ChunksPerFaceAxis &&
           a.radial>=0 && a.radial<PlanetSurface::RadialChunks;
}

bool ownsFlat(const PlanetChunkAddress& owner,int flat) {
    if(flat<0 || flat>=PlanetSurface::TotalCells) return false;
    const auto a=decodeFlat(flat);
    return chunkAddress(a.face,a.u,a.v,a.radial,PlanetSurface::ChunkSize)==owner;
}

std::string serializePayload(const SurfaceChunkRecord& record) {
    std::ostringstream out;
    out<<std::setprecision(9);
    out<<"SURFACE_CHUNK_PAYLOAD 8\n";
    out<<"planet_slot "<<record.planetSlot<<"\n";
    out<<"planet_seed "<<record.planetSeed<<"\n";
    out<<"generator_version "<<record.generatorVersion<<"\n";
    out<<"generator_fingerprint "<<record.generatorFingerprint<<"\n";
    out<<"save_generation "<<record.saveGeneration<<"\n";
    out<<"address "<<static_cast<int>(record.address.face)<<' '<<record.address.u<<' '<<record.address.v<<' '<<record.address.radial<<"\n";

    std::vector<std::pair<int,BlockType>> macro(record.journal.macroEdits.begin(),record.journal.macroEdits.end());
    std::sort(macro.begin(),macro.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    out<<"macro "<<macro.size()<<"\n";
    for(const auto& [idx,type]:macro) out<<"m "<<idx<<' '<<static_cast<int>(type)<<"\n";

    std::vector<int> placed(record.journal.placedMarkers.begin(),record.journal.placedMarkers.end());
    std::sort(placed.begin(),placed.end());
    out<<"placed "<<placed.size()<<"\n";
    for(const int idx:placed) out<<"p "<<idx<<"\n";

    std::vector<int> microCells;
    microCells.reserve(record.journal.microBricks.size());
    for(const auto& [idx,brick]:record.journal.microBricks) if(brick.overrideCount()>0) microCells.push_back(idx);
    std::sort(microCells.begin(),microCells.end());
    out<<"micro "<<microCells.size()<<"\n";
    for(const int idx:microCells) {
        const auto edits=record.journal.microBricks.at(idx).overrides();
        out<<"cell "<<idx<<' '<<static_cast<int>(record.journal.microBricks.at(idx).baseline())<<' '<<edits.size()<<"\n";
        for(const auto& [microIndex,type]:edits) out<<"e "<<microIndex<<' '<<static_cast<int>(type)<<"\n";
    }

    auto machines=record.machines;
    std::sort(machines.begin(),machines.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    out<<"machines "<<machines.size()<<"\n";
    for(const auto& o:machines) {
        out<<"machine "<<o.stableId<<' '<<static_cast<int>(o.type)<<' '
           <<static_cast<int>(o.anchor.face)<<' '<<o.anchor.u<<' '<<o.anchor.v<<' '<<o.anchor.radial<<' '
           <<(o.enabled?1:0)<<' '<<o.fuelSeconds<<' '<<o.storedEnergy<<' '<<o.roomPressure<<' '<<o.roomOxygen<<' '
           <<o.ammo<<' '<<o.shieldCharge<<' '<<o.selectedRecipeId<<' '<<o.activeRecipeId<<' '<<o.processProgressSeconds<<' '
           <<o.sorterFilterItemId<<' '<<o.logisticsSourceStableId<<' '<<o.logisticsTargetStableId<<' '
           <<o.logisticsAlternateTargetStableId<<' '<<o.logisticsProgressSeconds<<' '<<o.extractorProgressSeconds<<' '<<o.inventory.size()<<"\n";
        for(const auto& stack:o.inventory) out<<"item "<<stack.itemId<<' '<<stack.count<<"\n";
    }
    auto portals=record.portals;
    std::sort(portals.begin(),portals.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    out<<"portals "<<portals.size()<<"\n";
    for(const auto& portal:portals) {
        out<<"portal "<<portal.stableId<<' '<<static_cast<int>(portal.type)<<' '
           <<static_cast<int>(portal.anchor.face)<<' '<<portal.anchor.u<<' '<<portal.anchor.v<<' '<<portal.anchor.radial<<' '
           <<(portal.open?1:0)<<"\n";
    }
    auto airlocks=record.airlocks;
    std::sort(airlocks.begin(),airlocks.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    out<<"airlocks "<<airlocks.size()<<"\n";
    for(const auto& a:airlocks) {
        out<<"airlock "<<a.stableId<<' '<<a.controllerMachineId<<' '<<a.innerPortalId<<' '<<a.outerPortalId<<' '
           <<static_cast<int>(a.chamberAnchor.face)<<' '<<a.chamberAnchor.u<<' '<<a.chamberAnchor.v<<' '<<a.chamberAnchor.radial<<' '
           <<static_cast<int>(a.state)<<' '<<a.chamberPressure<<' '<<a.chamberOxygen<<"\n";
    }
    auto rules=record.automationRules;
    std::sort(rules.begin(),rules.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    out<<"automation "<<rules.size()<<"\n";
    for(const auto& rule:rules) {
        out<<"rule "<<rule.stableId<<' '<<rule.controllerMachineId<<' '<<static_cast<int>(rule.trigger)<<' '
           <<rule.sourceStableId<<' '<<rule.threshold<<' '<<static_cast<int>(rule.action)<<' '<<rule.targetStableId<<' '
           <<(rule.enabled?1:0)<<"\n";
    }
    out<<"END\n";
    return out.str();
}

std::optional<SurfaceChunkRecord> parsePayload(std::string_view payload,std::string& error) {
    std::istringstream in{std::string(payload)};
    std::string tag;
    int format{};
    SurfaceChunkRecord r{};
    if(!(in>>tag>>format) || tag!="SURFACE_CHUNK_PAYLOAD" || (format<1 || format>8)) { error="invalid payload header"; return std::nullopt; }
    if(!(in>>tag>>r.planetSlot) || tag!="planet_slot") {error="missing planet_slot";return std::nullopt;}
    if(!(in>>tag>>r.planetSeed) || tag!="planet_seed") {error="missing planet_seed";return std::nullopt;}
    if(!(in>>tag>>r.generatorVersion) || tag!="generator_version") {error="missing generator_version";return std::nullopt;}
    if(!(in>>tag>>r.generatorFingerprint) || tag!="generator_fingerprint") {error="missing generator_fingerprint";return std::nullopt;}
    if(format>=2) {
        if(!(in>>tag>>r.saveGeneration) || tag!="save_generation") {error="missing save_generation";return std::nullopt;}
    } else {
        r.saveGeneration=0;
    }
    int face{};
    if(!(in>>tag>>face>>r.address.u>>r.address.v>>r.address.radial) || tag!="address" || face<0 || face>=PlanetSurface::FaceCount) {error="invalid address";return std::nullopt;}
    r.address.face=static_cast<CubeFace>(face);
    r.journal.address=r.address;

    std::size_t count{};
    if(!(in>>tag>>count) || tag!="macro") {error="missing macro section";return std::nullopt;}
    for(std::size_t i=0;i<count;++i) {
        int idx{},type{};
        if(!(in>>tag>>idx>>type) || tag!="m" || type<0 || type>=kBlockTypeCount) {error="invalid macro entry";return std::nullopt;}
        r.journal.macroEdits[idx]=static_cast<BlockType>(type);
    }
    if(!(in>>tag>>count) || tag!="placed") {error="missing placed section";return std::nullopt;}
    for(std::size_t i=0;i<count;++i) {
        int idx{};
        if(!(in>>tag>>idx) || tag!="p") {error="invalid placed entry";return std::nullopt;}
        r.journal.placedMarkers.insert(idx);
    }
    if(!(in>>tag>>count) || tag!="micro") {error="missing micro section";return std::nullopt;}
    for(std::size_t i=0;i<count;++i) {
        int idx{},baseline{}; std::size_t edits{};
        if(!(in>>tag>>idx>>baseline>>edits) || tag!="cell" || baseline<0 || baseline>=kBlockTypeCount) {error="invalid micro cell";return std::nullopt;}
        MicroBrick brick(static_cast<BlockType>(baseline));
        for(std::size_t e=0;e<edits;++e) {
            int microIndex{},type{};
            if(!(in>>tag>>microIndex>>type) || tag!="e" || microIndex<0 || microIndex>=MicroBrick::CellCount || type<0 || type>=kBlockTypeCount) {error="invalid micro override";return std::nullopt;}
            brick.setIndex(microIndex,static_cast<BlockType>(type));
        }
        r.journal.microBricks[idx]=std::move(brick);
    }
    if(!(in>>tag>>count) || tag!="machines") {error="missing machines section";return std::nullopt;}
    for(std::size_t i=0;i<count;++i) {
        SurfaceMachineObject o{}; int type{},mf{},enabled{};
        if(!(in>>tag>>o.stableId>>type>>mf>>o.anchor.u>>o.anchor.v>>o.anchor.radial>>enabled>>o.fuelSeconds>>o.storedEnergy) || tag!="machine") {error="invalid machine entry";return std::nullopt;}
        if(format>=3) {
            if(!(in>>o.roomPressure>>o.roomOxygen)) {error="invalid machine atmosphere state";return std::nullopt;}
            if(!std::isfinite(o.roomPressure) || !std::isfinite(o.roomOxygen) || o.roomPressure<0.0f || o.roomPressure>1.0f || o.roomOxygen<0.0f || o.roomOxygen>1.0f) {
                error="machine atmosphere state out of range";return std::nullopt;
            }
        }
        if(format>=5) {
            if(!(in>>o.ammo>>o.shieldCharge)) {error="invalid machine defense state";return std::nullopt;}
            if(o.ammo<0 || !std::isfinite(o.shieldCharge) || o.shieldCharge<0.0f || o.shieldCharge>120.0f) {error="machine defense state out of range";return std::nullopt;}
        }
        if(format>=6) {
            std::size_t inventoryCount{};
            if(format>=8) {
                if(!(in>>o.selectedRecipeId>>o.activeRecipeId>>o.processProgressSeconds>>o.sorterFilterItemId
                     >>o.logisticsSourceStableId>>o.logisticsTargetStableId>>o.logisticsAlternateTargetStableId
                     >>o.logisticsProgressSeconds>>o.extractorProgressSeconds>>inventoryCount)) {error="invalid machine extractor/logistics/industry state";return std::nullopt;}
            } else if(format>=7) {
                if(!(in>>o.selectedRecipeId>>o.activeRecipeId>>o.processProgressSeconds>>o.sorterFilterItemId
                     >>o.logisticsSourceStableId>>o.logisticsTargetStableId>>o.logisticsAlternateTargetStableId
                     >>o.logisticsProgressSeconds>>inventoryCount)) {error="invalid machine logistics/industry state";return std::nullopt;}
            } else {
                if(!(in>>o.selectedRecipeId>>o.activeRecipeId>>o.processProgressSeconds>>o.sorterFilterItemId>>inventoryCount)) {error="invalid machine industry state";return std::nullopt;}
            }
            if(o.selectedRecipeId<0 || o.activeRecipeId<0 || o.sorterFilterItemId<0 || !std::isfinite(o.processProgressSeconds) || o.processProgressSeconds<0.0f ||
               !std::isfinite(o.logisticsProgressSeconds) || o.logisticsProgressSeconds<0.0f ||
               !std::isfinite(o.extractorProgressSeconds) || o.extractorProgressSeconds<0.0f || inventoryCount>64) {error="machine industry state out of range";return std::nullopt;}
            o.inventory.reserve(inventoryCount);
            int previousItem=0;
            for(std::size_t stackIndex=0;stackIndex<inventoryCount;++stackIndex) {
                SurfaceItemStack stack{};
                if(!(in>>tag>>stack.itemId>>stack.count) || tag!="item" || stack.itemId<=previousItem || stack.count<=0 || stack.count>999) {error="invalid machine inventory entry";return std::nullopt;}
                previousItem=stack.itemId;
                o.inventory.push_back(stack);
            }
        }
        if(type<0 || type>static_cast<int>(MachineType::ArcSmelter) || mf<0 || mf>=PlanetSurface::FaceCount) {error="machine enum out of range";return std::nullopt;}
        o.type=static_cast<MachineType>(type); o.anchor.face=static_cast<CubeFace>(mf); o.enabled=enabled!=0;
        r.machines.push_back(o);
    }
    if(format>=3) {
        if(!(in>>tag>>count) || tag!="portals") {error="missing portals section";return std::nullopt;}
        for(std::size_t i=0;i<count;++i) {
            SurfacePortalObject portal{}; int type{},pf{},open{};
            if(!(in>>tag>>portal.stableId>>type>>pf>>portal.anchor.u>>portal.anchor.v>>portal.anchor.radial>>open) || tag!="portal") {error="invalid portal entry";return std::nullopt;}
            if(type<0 || type>static_cast<int>(SurfacePortalType::Airlock) || pf<0 || pf>=PlanetSurface::FaceCount) {error="portal enum out of range";return std::nullopt;}
            portal.type=static_cast<SurfacePortalType>(type); portal.anchor.face=static_cast<CubeFace>(pf); portal.open=open!=0;
            r.portals.push_back(portal);
        }
    }
    if(format>=4) {
        if(!(in>>tag>>count) || tag!="airlocks") {error="missing airlocks section";return std::nullopt;}
        for(std::size_t i=0;i<count;++i) {
            SurfaceAirlockAssembly a{}; int af{},state{};
            if(!(in>>tag>>a.stableId>>a.controllerMachineId>>a.innerPortalId>>a.outerPortalId>>af>>a.chamberAnchor.u>>a.chamberAnchor.v>>a.chamberAnchor.radial>>state>>a.chamberPressure>>a.chamberOxygen) || tag!="airlock") {error="invalid airlock assembly entry";return std::nullopt;}
            if(af<0 || af>=PlanetSurface::FaceCount || state<0 || state>static_cast<int>(SurfaceAirlockState::Fault)) {error="airlock assembly enum out of range";return std::nullopt;}
            if(!std::isfinite(a.chamberPressure) || !std::isfinite(a.chamberOxygen) || a.chamberPressure<0.0f || a.chamberPressure>1.0f || a.chamberOxygen<0.0f || a.chamberOxygen>1.0f) {error="airlock atmosphere state out of range";return std::nullopt;}
            a.chamberAnchor.face=static_cast<CubeFace>(af); a.state=static_cast<SurfaceAirlockState>(state);
            r.airlocks.push_back(a);
        }
    }
    if(format>=5) {
        if(!(in>>tag>>count) || tag!="automation") {error="missing automation section";return std::nullopt;}
        for(std::size_t i=0;i<count;++i) {
            SurfaceAutomationRule rule{}; int trigger{},action{},enabled{};
            if(!(in>>tag>>rule.stableId>>rule.controllerMachineId>>trigger>>rule.sourceStableId>>rule.threshold>>action>>rule.targetStableId>>enabled) || tag!="rule") {error="invalid automation rule entry";return std::nullopt;}
            if(trigger<0 || trigger>static_cast<int>(SurfaceAutomationTrigger::AtmospherePressureBelow) || action<0 || action>static_cast<int>(SurfaceAutomationAction::ClosePortal) || !std::isfinite(rule.threshold) || rule.threshold<0.0f) {error="automation rule state out of range";return std::nullopt;}
            rule.trigger=static_cast<SurfaceAutomationTrigger>(trigger); rule.action=static_cast<SurfaceAutomationAction>(action); rule.enabled=enabled!=0;
            r.automationRules.push_back(rule);
        }
    }
    if(!(in>>tag) || tag!="END") {error="missing END";return std::nullopt;}
    return r;
}

} // namespace

namespace {

bool writeAtomicGenerationImpl(const std::filesystem::path& destination,std::string_view bytes,
                               bool rotatePrevious,std::string* error) {
    auto temp=destination; temp += ".tmp";
    auto previous=destination; previous += ".prev";
    std::error_code ec;
    if(destination.has_parent_path() && !destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(),ec);
        if(ec) {if(error)*error="could not create save directory: "+ec.message();return false;}
    }
    std::filesystem::remove(temp,ec); ec.clear();
    if(!writeAllDurable(temp,bytes,error)) {std::filesystem::remove(temp,ec);return false;}

    bool rotated=false;
    if(std::filesystem::exists(destination)) {
        if(rotatePrevious) {
            std::filesystem::remove(previous,ec); ec.clear();
            std::filesystem::rename(destination,previous,ec);
            if(ec) {if(error)*error="could not rotate previous generation: "+ec.message();std::filesystem::remove(temp,ec);return false;}
            rotated=true;
        } else {
            // Replacing an uncommitted retry of the same save generation must
            // not rotate away the previous manifest-compatible chunk.
            std::filesystem::remove(destination,ec);
            if(ec) {if(error)*error="could not replace uncommitted generation: "+ec.message();std::filesystem::remove(temp,ec);return false;}
        }
    }

    std::filesystem::rename(temp,destination,ec);
    if(ec) {
        if(rotated && std::filesystem::exists(previous)) {
            std::error_code restore;
            std::filesystem::rename(previous,destination,restore);
        }
        if(error)*error="could not atomically publish generation: "+ec.message();
        std::filesystem::remove(temp,ec);
        return false;
    }
    return true;
}


} // namespace

bool writeAtomicGeneration(const std::filesystem::path& destination,std::string_view bytes,std::string* error) {
    return writeAtomicGenerationImpl(destination,bytes,true,error);
}


bool writeCheckedAtomicGeneration(const std::filesystem::path& destination,std::string_view payload,std::string* error) {
    std::ostringstream wrapper;
    wrapper<<"ELYSIUM_ATOMIC_GENERATION 1\n";
    wrapper<<"payload_bytes "<<payload.size()<<"\n";
    wrapper<<"checksum "<<checksum64(payload)<<"\n\n";
    wrapper<<payload;

    // If the current manifest is corrupt but its retained .prev is valid, do
    // not rotate the corrupt current over the known-good fallback. Plain legacy
    // manifests are considered valid current generations for upgrade purposes.
    bool rotatePrevious=true;
    if(std::filesystem::exists(destination)) {
        const auto current=readCheckedPath(destination,true);
        auto previous=destination; previous += ".prev";
        if(!current.loaded && std::filesystem::exists(previous)) rotatePrevious=false;
    }
    return writeAtomicGenerationImpl(destination,wrapper.str(),rotatePrevious,error);
}

CheckedGenerationReadResult readCheckedAtomicGeneration(const std::filesystem::path& destination,bool allowPlainLegacy) {
    auto current=readCheckedPath(destination,allowPlainLegacy);
    if(current.loaded) return current;
    auto previous=destination; previous += ".prev";
    auto fallback=readCheckedPath(previous,allowPlainLegacy);
    if(fallback.loaded) {
        fallback.usedPreviousGeneration=true;
        fallback.error="current generation invalid; recovered previous known-good generation";
        return fallback;
    }
    if(!current.error.empty()&&!fallback.error.empty()) current.error += "; previous: "+fallback.error;
    return current;
}

SurfaceChunkStore::SurfaceChunkStore(std::filesystem::path root,int planetSlot,std::uint64_t planetSeed,
                                     int generatorVersion,std::uint64_t generatorFingerprint)
    : root_(std::move(root)),planetSlot_(planetSlot),planetSeed_(planetSeed),
      generatorVersion_(generatorVersion),generatorFingerprint_(generatorFingerprint) {}

std::filesystem::path SurfaceChunkStore::recordPath(const PlanetChunkAddress& address) const {
    std::ostringstream name;
    name<<std::hex<<std::setw(16)<<std::setfill('0')<<stableChunkKey(address)<<".chunk";
    return root_ / (std::string("planet_")+std::to_string(planetSlot_)) / name.str();
}

std::filesystem::path SurfaceChunkStore::previousPath(const PlanetChunkAddress& address) const {
    auto p=recordPath(address);
    p += ".prev";
    return p;
}

bool SurfaceChunkStore::save(const SurfaceChunkRecord& record,std::string* error) const {
    if(record.planetSlot!=planetSlot_ || record.planetSeed!=planetSeed_ ||
       record.generatorVersion!=generatorVersion_ || record.generatorFingerprint!=generatorFingerprint_ ||
       !validChunkAddress(record.address) || !(record.journal.address==record.address)) {
        if(error) *error="chunk record identity does not match store contract";
        return false;
    }
    for(const auto& [idx,_]:record.journal.macroEdits) if(!ownsFlat(record.address,idx)) {if(error)*error="macro edit belongs to another chunk";return false;}
    for(const int idx:record.journal.placedMarkers) if(!ownsFlat(record.address,idx)) {if(error)*error="placed marker belongs to another chunk";return false;}
    for(const auto& [idx,_]:record.journal.microBricks) if(!ownsFlat(record.address,idx)) {if(error)*error="microbrick belongs to another chunk";return false;}
    for(const auto& o:record.machines) {
        if(o.anchor.u<0||o.anchor.u>=PlanetSurface::FaceResolution||o.anchor.v<0||o.anchor.v>=PlanetSurface::FaceResolution||o.anchor.radial<0||o.anchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(o.anchor.face,o.anchor.u,o.anchor.v,o.anchor.radial,PlanetSurface::ChunkSize)==record.address)) {
            if(error) *error="machine belongs to another chunk";
            return false;
        }
    }
    for(const auto& portal:record.portals) {
        if(portal.anchor.u<0||portal.anchor.u>=PlanetSurface::FaceResolution||portal.anchor.v<0||portal.anchor.v>=PlanetSurface::FaceResolution||portal.anchor.radial<0||portal.anchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(portal.anchor.face,portal.anchor.u,portal.anchor.v,portal.anchor.radial,PlanetSurface::ChunkSize)==record.address)) {
            if(error) *error="portal belongs to another chunk";
            return false;
        }
    }
    for(const auto& a:record.airlocks) {
        if(a.chamberAnchor.u<0||a.chamberAnchor.u>=PlanetSurface::FaceResolution||a.chamberAnchor.v<0||a.chamberAnchor.v>=PlanetSurface::FaceResolution||a.chamberAnchor.radial<0||a.chamberAnchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(a.chamberAnchor.face,a.chamberAnchor.u,a.chamberAnchor.v,a.chamberAnchor.radial,PlanetSurface::ChunkSize)==record.address)) {
            if(error) *error="airlock assembly belongs to another chunk";
            return false;
        }
    }
    for(const auto& rule:record.automationRules) {
        const auto controller=std::find_if(record.machines.begin(),record.machines.end(),[&](const auto& m){return m.stableId==rule.controllerMachineId;});
        if(controller==record.machines.end() || controller->type!=MachineType::LogicController) {
            if(error) *error="automation rule controller is not owned by this chunk";
            return false;
        }
    }

    const std::string payload=serializePayload(record);
    std::ostringstream wrapper;
    wrapper<<"ELYSIUM_CHUNK_TXN "<<RecordFormatVersion<<"\n";
    wrapper<<"payload_bytes "<<payload.size()<<"\n";
    wrapper<<"checksum "<<checksum64(payload)<<"\n\n";
    wrapper<<payload;

    // If a prior save attempt already wrote this same whole-save generation
    // but failed before publishing the manifest, replace that uncommitted
    // current file without rotating away the manifest-compatible .prev copy.
    bool rotatePrevious=true;
    const auto currentPath=recordPath(record.address);
    if(std::filesystem::exists(currentPath)) {
        const auto existing=loadPath(currentPath,record.address,std::nullopt);
        if(record.saveGeneration!=0 && existing.loaded && existing.record.saveGeneration==record.saveGeneration) rotatePrevious=false;
        else if(!existing.loaded && std::filesystem::exists(previousPath(record.address))) rotatePrevious=false;
    }
    return writeAtomicGenerationImpl(currentPath,wrapper.str(),rotatePrevious,error);
}

SurfaceChunkLoadResult SurfaceChunkStore::loadPath(const std::filesystem::path& path,const PlanetChunkAddress& expected,
                                                   std::optional<std::uint64_t> expectedSaveGeneration) const {
    SurfaceChunkLoadResult result{};
    std::ifstream in(path,std::ios::binary);
    if(!in){result.error="chunk file unavailable";return result;}
    std::string line;
    if(!std::getline(in,line)){result.error="missing transaction header";return result;}
    {std::istringstream ls(line);std::string tag;int ver{};if(!(ls>>tag>>ver)||tag!="ELYSIUM_CHUNK_TXN"||(ver<1 || ver>RecordFormatVersion)){result.error="invalid transaction header";return result;}}
    std::size_t payloadBytes{};
    if(!std::getline(in,line)){result.error="missing payload size";return result;}
    {std::istringstream ls(line);std::string tag;if(!(ls>>tag>>payloadBytes)||tag!="payload_bytes"){result.error="invalid payload size";return result;}}
    std::uint64_t expectedChecksum{};
    if(!std::getline(in,line)){result.error="missing checksum";return result;}
    {std::istringstream ls(line);std::string tag;if(!(ls>>tag>>expectedChecksum)||tag!="checksum"){result.error="invalid checksum header";return result;}}
    if(!std::getline(in,line) || !line.empty()){result.error="missing transaction delimiter";return result;}
    std::string payload(payloadBytes,'\0');
    in.read(payload.data(),static_cast<std::streamsize>(payloadBytes));
    if(static_cast<std::size_t>(in.gcount())!=payloadBytes){result.error="truncated chunk payload";return result;}
    if(checksum64(payload)!=expectedChecksum){result.error="chunk checksum mismatch";return result;}
    std::string parseError;
    auto parsed=parsePayload(payload,parseError);
    if(!parsed){result.error=parseError;return result;}
    auto& r=*parsed;
    if(r.planetSlot!=planetSlot_ || r.planetSeed!=planetSeed_ || r.generatorVersion!=generatorVersion_ ||
       r.generatorFingerprint!=generatorFingerprint_ || !(r.address==expected) || !validChunkAddress(r.address)) {
        result.error="chunk compatibility/identity mismatch";return result;
    }
    if(expectedSaveGeneration && r.saveGeneration!=*expectedSaveGeneration) {
        result.error="chunk save-generation mismatch";return result;
    }
    if(!(r.journal.address==r.address)){result.error="journal address mismatch";return result;}
    for(const auto& [idx,_]:r.journal.macroEdits) if(!ownsFlat(r.address,idx)){result.error="macro record crosses chunk ownership";return result;}
    for(const int idx:r.journal.placedMarkers) if(!ownsFlat(r.address,idx)){result.error="placed record crosses chunk ownership";return result;}
    for(const auto& [idx,_]:r.journal.microBricks) if(!ownsFlat(r.address,idx)){result.error="micro record crosses chunk ownership";return result;}
    for(const auto& o:r.machines) {
        if(o.anchor.u<0||o.anchor.u>=PlanetSurface::FaceResolution||o.anchor.v<0||o.anchor.v>=PlanetSurface::FaceResolution||o.anchor.radial<0||o.anchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(o.anchor.face,o.anchor.u,o.anchor.v,o.anchor.radial,PlanetSurface::ChunkSize)==r.address)) {
            result.error="machine record crosses chunk ownership";return result;
        }
    }
    for(const auto& portal:r.portals) {
        if(portal.anchor.u<0||portal.anchor.u>=PlanetSurface::FaceResolution||portal.anchor.v<0||portal.anchor.v>=PlanetSurface::FaceResolution||portal.anchor.radial<0||portal.anchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(portal.anchor.face,portal.anchor.u,portal.anchor.v,portal.anchor.radial,PlanetSurface::ChunkSize)==r.address)) {
            result.error="portal record crosses chunk ownership";return result;
        }
    }
    for(const auto& a:r.airlocks) {
        if(a.chamberAnchor.u<0||a.chamberAnchor.u>=PlanetSurface::FaceResolution||a.chamberAnchor.v<0||a.chamberAnchor.v>=PlanetSurface::FaceResolution||a.chamberAnchor.radial<0||a.chamberAnchor.radial>=PlanetSurface::RadialLayers ||
           !(chunkAddress(a.chamberAnchor.face,a.chamberAnchor.u,a.chamberAnchor.v,a.chamberAnchor.radial,PlanetSurface::ChunkSize)==r.address)) {
            result.error="airlock assembly record crosses chunk ownership";return result;
        }
    }
    for(const auto& rule:r.automationRules) {
        const auto controller=std::find_if(r.machines.begin(),r.machines.end(),[&](const auto& m){return m.stableId==rule.controllerMachineId;});
        if(controller==r.machines.end() || controller->type!=MachineType::LogicController) {
            result.error="automation rule controller is not owned by record";return result;
        }
    }
    result.loaded=true;
    result.record=std::move(r);
    return result;
}

SurfaceChunkLoadResult SurfaceChunkStore::load(const PlanetChunkAddress& address,
                                               std::optional<std::uint64_t> expectedSaveGeneration) const {
    auto current=loadPath(recordPath(address),address,expectedSaveGeneration);
    if(current.loaded) return current;
    auto previous=loadPath(previousPath(address),address,expectedSaveGeneration);
    if(previous.loaded) {
        previous.usedPreviousGeneration=true;
        previous.error="current generation invalid/incompatible; recovered previous manifest-compatible chunk generation";
        return previous;
    }
    if(!current.error.empty() && !previous.error.empty()) current.error += "; previous: "+previous.error;
    return current;
}


std::vector<SurfaceManifestChunkRef> extractSurfaceManifestChunkRefs(std::string_view manifestPayload) {
    std::vector<SurfaceManifestChunkRef> refs;
    std::istringstream in{std::string(manifestPayload)};
    std::string tag;
    while(in>>tag) {
        if(tag!="surface_manifest") continue;
        int planetSlot{}; std::size_t count{};
        if(!(in>>planetSlot>>count)) break;
        for(std::size_t i=0;i<count;++i) {
            int face{},u{},v{},radial{};
            if(!(in>>tag>>face>>u>>v>>radial) || tag!="surface_chunk") return refs;
            if(planetSlot<0 || face<0 || face>=PlanetSurface::FaceCount) continue;
            const PlanetChunkAddress address{static_cast<CubeFace>(face),u,v,radial};
            if(validChunkAddress(address)) refs.push_back({planetSlot,address});
        }
    }
    return refs;
}

bool pruneSurfaceChunkStore(const std::filesystem::path& root,
                            const std::vector<SurfaceManifestChunkRef>& currentRefs,
                            const std::vector<SurfaceManifestChunkRef>& previousRefs,
                            std::string* error) {
    std::unordered_set<std::string> keep;
    auto add=[&](const SurfaceManifestChunkRef& ref) {
        std::ostringstream name;
        name<<std::hex<<std::setw(16)<<std::setfill('0')<<stableChunkKey(ref.address)<<".chunk";
        const auto path=(root/(std::string("planet_")+std::to_string(ref.planetSlot))/name.str()).lexically_normal();
        keep.insert(path.generic_string());
        auto previous=path; previous += ".prev";
        // A referenced chunk may physically live in current or .prev depending
        // on whether it was rewritten by the newer manifest generation. Keep
        // both slots; generation matching on load chooses the correct one.
        keep.insert(previous.generic_string());
    };
    for(const auto& ref:currentRefs) add(ref);
    for(const auto& ref:previousRefs) add(ref);

    std::error_code ec;
    if(!std::filesystem::exists(root,ec)) return true;
    for(std::filesystem::recursive_directory_iterator it(root,ec),end; it!=end && !ec; ++it) {
        if(!it->is_regular_file()) continue;
        const auto path=it->path().lexically_normal();
        const auto text=path.generic_string();
        const auto filename=path.filename().string();
        const bool temp=filename.ends_with(".tmp");
        const bool sidecar=filename.ends_with(".chunk") || filename.ends_with(".chunk.prev");
        if(temp || (sidecar && !keep.contains(text))) {
            std::filesystem::remove(path,ec);
            if(ec) { if(error) *error="could not prune stale surface sidecar: "+ec.message(); return false; }
        }
    }
    if(ec) { if(error) *error="could not scan surface sidecar store: "+ec.message(); return false; }

    // Remove empty planet directories left after stale transaction cleanup.
    for(auto it=std::filesystem::directory_iterator(root,ec); it!=std::filesystem::directory_iterator() && !ec; ++it) {
        if(it->is_directory() && std::filesystem::is_empty(it->path(),ec)) {
            std::filesystem::remove(it->path(),ec);
            if(ec) { if(error) *error="could not remove empty sidecar directory: "+ec.message(); return false; }
        }
    }
    return true;
}

bool SurfaceChunkStore::remove(const PlanetChunkAddress& address,std::string* error) const {
    std::error_code ec;
    std::filesystem::remove(recordPath(address),ec);
    if(ec){if(error)*error=ec.message();return false;}
    std::filesystem::remove(previousPath(address),ec);
    if(ec){if(error)*error=ec.message();return false;}
    return true;
}

SurfaceChunkRecord makeSurfaceChunkRecord(int planetSlot,const PlanetSurface& surface,
                                          const SurfaceInfrastructure& infrastructure,
                                          const PlanetChunkAddress& address,
                                          std::uint64_t saveGeneration) {
    SurfaceChunkRecord r{};
    r.planetSlot=planetSlot;
    r.planetSeed=surface.seed();
    r.generatorVersion=kSurfaceGeneratorVersion;
    r.generatorFingerprint=kSurfaceGeneratorFingerprint;
    r.saveGeneration=saveGeneration;
    r.address=address;
    r.journal.address=address;
    if(const auto* j=surface.journal(address)) r.journal=*j;
    for(const auto& o:infrastructure.objects()) if(surface.chunkOf(o.anchor)==address) r.machines.push_back(o);
    for(const auto& portal:infrastructure.portals()) if(surface.chunkOf(portal.anchor)==address) r.portals.push_back(portal);
    for(const auto& a:infrastructure.airlockAssemblies()) if(surface.chunkOf(a.chamberAnchor)==address) r.airlocks.push_back(a);
    for(const auto& rule:infrastructure.automationRules()) {
        const auto* controller=infrastructure.find(rule.controllerMachineId);
        if(controller && surface.chunkOf(controller->anchor)==address) r.automationRules.push_back(rule);
    }
    return r;
}

} // namespace elysium
