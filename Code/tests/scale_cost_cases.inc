#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <entt/entt.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ScratchAllocator.hpp>
#include <Packet.hpp>
#include <snappy.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#undef interface
#include <string_view>
using namespace TiltedPhoques;
namespace spdlog {
std::atomic<uint32_t> writerStacks{};
std::atomic<uint32_t> dispatchStacks{};
std::function<void()> writerLog;
template<class... T> void error(T&&...) {}
template<class... T> void info(const char* format, T&&...) {
    if (std::string_view(format).starts_with("Scale lane: writer-stack")) {
        ++writerStacks;
        if (writerLog) writerLog();
    }
    if (std::string_view(format).starts_with("Scale lane: dispatch-stack")) ++dispatchStacks;
}
}
using ConnectionId_t = uint32_t;
struct Player {
    explicit Player(uint32_t c) : connection(c), id(++next) {}
    uint32_t GetId() const { return id; }
    uint32_t GetConnectionId() const { return connection; }
    uint32_t GetStringCacheId() const { return baseline; }
    const String& GetUsername() const { return name; }
    uint32_t connection, id, baseline{};
    String name;
    static inline uint32_t next{};
};
// INSERT PLAYER MANAGER HEADER
// INSERT PLAYER MANAGER SOURCE
void PlayerTests() {
    PlayerManager manager;
    std::array<uint32_t, 10> ids{};
    for (uint32_t i = 0; i < 10; ++i) {
        auto* p = manager.Create(i); assert(p); ids[i] = p->GetId();
        assert(!manager.Create(i));
        assert(manager.GetById(ids[i]) == p);
        assert(std::as_const(manager).GetById(ids[i]) == p);
    }
    for (uint32_t i : {0, 4, 9}) {
        manager.Remove(manager.GetById(ids[i]));
        assert(!manager.GetById(ids[i]));
        auto* p = manager.Create(i); assert(p && p->GetId() != ids[i]);
        assert(manager.GetByConnectionId(i) == manager.GetById(p->GetId()));
    }
    assert(manager.Count() == 10 && !manager.GetById(0));
}

namespace TiltedPhoques {
constexpr char kPayload = 0, kCompressedPayload = 2;
enum EPacketFlags { kReliable, kUnreliable };
constexpr int k_nSteamNetworkingSend_Reliable = 8, k_nSteamNetworkingSend_Unreliable = 0;
// Only the final Steam dispatch is replaced; compression uses production Send
// and the installed Snappy library, independently of the broadcast helper.
struct Server {
    struct Interface {
        std::vector<std::pair<uint32_t, std::vector<char>>> sent;
        void SendMessageToConnection(uint32_t connection, const void* bytes, uint32_t size, int, void*) {
            const auto* data = static_cast<const char*>(bytes);
            sent.push_back({connection, {data, data + size}});
        }
    } interface;
    Interface* m_pInterface{&interface};
    auto& Sent() { return interface.sent; }
    void Send(uint32_t connection, Packet* packet, EPacketFlags flags = kReliable) const noexcept;
};
// INSERT REAL TRANSPORT SEND
}
// INSERT PACKET SOURCE
struct ServerMessage {
    mutable uint32_t serializations{};
    size_t size{32};
    uint8_t salt{};
    void Serialize(Buffer::Writer& w) const {
        ++serializations;
        for (size_t i = 0; i < size; ++i) assert(w.WriteBits((i % 4) ^ salt, 8));
    }
};
// INSERT SEND HELPERS
void SendTests() {
    const void* address{};
    auto inspect = [&](auto& packet) {
        if (address) assert(address == packet.GetData());
        address = packet.GetData();
        assert(packet.GetSize() == 32);
    };
    ServerMessage message;
    for (int i = 0; i < 100; ++i) WithEncodedServerMessage(message, inspect);
    assert(Allocator::Get() == Allocator::GetDefault());
    WithEncodedServerMessage(message, [&](auto& outer) {
        const std::vector<char> original(outer.GetData(), outer.GetData() + outer.GetSize());
        const auto* outerData = outer.GetData();
        WithEncodedServerMessage(message, [&](auto& inner) {
            assert(inner.GetData() != outerData);
            inner.GetData()[0] = 99;
        });
        assert(std::equal(original.begin(), original.end(), outer.GetData()));
    });
    assert(Allocator::Get() == Allocator::GetDefault() && ServerSendDepth() == 0);
    for (uint32_t count : {5, 10}) for (size_t size : {size_t{3}, size_t{512}}) {
        Server server;
        message.size = size; message.serializations = 0;
        std::vector<Player> players;
        for (uint32_t i = 0; i < count; ++i) {
            players.emplace_back(i);
            players.back().baseline = i % 3; // Interleaved connection baselines.
        }
        ServerBroadcast broadcast{server, message, {}};
        assert(message.serializations == 0); // No recipients means no encode.
        for (const auto& p : players) { message.salt=uint8_t(p.baseline); broadcast.Send(&p); }
        assert(message.serializations == 3 && server.Sent().size() == count);
        for (uint32_t i = 0; i < count; ++i) {
            assert(server.Sent()[i].first == i);
            Server direct;
            message.salt=uint8_t(players[i].baseline);
            WithEncodedServerMessage(message, [&](auto& packet) { direct.Send(i, &packet); });
            assert(server.Sent()[i].second == direct.Sent()[0].second);
            assert(server.Sent()[i].second.front() == (size > 8 ? 2 : 0));
        }
        ServerBroadcast next{server, message, {}};
        const auto previous = message.serializations;
        next.Send(&players[0]); assert(message.serializations == previous+1);
    }
}

namespace entity_tests {
struct LocalComponent { uint32_t Id{}, OwnershipEpoch{}; };
struct RemoteComponent { uint32_t Id{}, CachedRefId{}, OwnershipEpoch{}; };
struct FormIdComponent { uint32_t Id{}; };
struct ObjectComponent { uint32_t Id{}; };
struct InterpolationComponent { int value{}; };
struct RemoteAnimationComponent { int value{}; };
struct World : entt::registry { static World& Get() { static World world; return world; } };
struct ActorOwnershipToken { uint32_t ServerId{}, OwnershipEpoch{}; };
struct EntityDispatchScope { EntityDispatchScope() noexcept; ~EntityDispatchScope() noexcept; };
// INSERT ENTITY INDEX
void Run() {
    auto& world = World::Get();
    InitializeEntityIndex(world);
    const EntityDispatchScope dispatch;
    auto check = [&] {
        for (uint32_t id = 0; id < 17; ++id) {
            std::optional<entt::entity> expected;
            for (auto e : world.view<LocalComponent>()) if (world.get<LocalComponent>(e).Id == id) { expected=e; break; }
            if (!expected) for (auto e : world.view<RemoteComponent>()) if (world.get<RemoteComponent>(e).Id == id) { expected=e; break; }
            if (!expected) for (auto e : world.view<ObjectComponent>()) if (world.get<ObjectComponent>(e).Id == id) { expected=e; break; }
            assert(FindEntityByServerId(id) == expected);
            assert(FindEntityByServerIdOnRunner(id) == expected);
            expected.reset();
            for (auto e : world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>())
                if (world.get<RemoteComponent>(e).Id == id) { expected=e; break; }
            assert(FindMovementEntityByServerId(id) == expected);
            expected.reset();
            for (auto e : world.view<FormIdComponent, LocalComponent>()) if (world.get<FormIdComponent>(e).Id == id) { expected=e; break; }
            assert(FindLocalEntityByFormId(id) == expected);
            const auto token = GetLocalOwnershipToken(id);
            assert(bool(token) == bool(expected && world.get<LocalComponent>(*expected).OwnershipEpoch));
            if (token) assert(token->OwnershipEpoch == world.get<LocalComponent>(*expected).OwnershipEpoch);
            const auto indexedLocal = GetLocalOwnershipTokenOnRunner(id);
            assert(bool(indexedLocal) == bool(token));
            if (token) assert(indexedLocal->ServerId == token->ServerId && indexedLocal->OwnershipEpoch == token->OwnershipEpoch);
            const auto remoteToken = GetRemoteOwnershipToken(id);
            const auto indexedRemote = GetRemoteOwnershipTokenOnRunner(id);
            assert(bool(indexedRemote) == bool(remoteToken));
            if (remoteToken) assert(indexedRemote->ServerId == remoteToken->ServerId && indexedRemote->OwnershipEpoch == remoteToken->OwnershipEpoch);
            expected.reset();
            for (auto e : world.view<RemoteComponent, FormIdComponent>()) if (world.get<RemoteComponent>(e).Id == id) { expected=e; break; }
            assert(FindRemoteActorByServerId(id) == expected);
        }
    };
    std::vector<entt::entity> actors;
    uint32_t rng = 19287;
    for (uint32_t step=0; step<10000; ++step) {
        rng = rng * 1664525u + 1013904223u;
        if (actors.empty() || (rng % 13 == 0 && actors.size() < 80)) actors.push_back(world.create());
        auto e = actors[(rng >> 16) % actors.size()];
        const uint32_t id = (rng >> 8) % 17;
        switch (rng % 11) {
        case 0: world.emplace_or_replace<RemoteComponent>(e, id, id+100, step); break;
        case 1: world.emplace_or_replace<LocalComponent>(e, id, step % 7); break;
        case 2: world.emplace_or_replace<FormIdComponent>(e, id); break;
        case 3: world.emplace_or_replace<InterpolationComponent>(e); break;
        case 4: world.emplace_or_replace<RemoteAnimationComponent>(e); break;
        case 5: world.remove<RemoteComponent>(e); break;
        case 6: world.remove<LocalComponent>(e); break;
        case 7: world.remove<FormIdComponent>(e); break;
        case 8: world.emplace_or_replace<ObjectComponent>(e, id); break;
        case 9: if (world.all_of<RemoteComponent>(e)) world.patch<RemoteComponent>(e, [&](auto& r) { ++r.OwnershipEpoch; }); break;
        case 10: world.destroy(e); std::erase(actors, e); break;
        }
        check();
    }
    world.clear(); check();
    // Index can also bootstrap a populated registry, and disconnects before pools die.
    entt::registry late;
    const auto e = late.create(); late.emplace<RemoteComponent>(e, 2u, 22u, 3u);
    late.emplace<FormIdComponent>(e, 22u);
    auto& index = EntityIndex(late); assert(index.RemoteActors.Find(2) == e);
    late.clear(); assert(!index.RemoteActors.Find(2));
    // Unique-key readers touch only the protected cache while lifecycle updates
    // run on the registry lane. Native ownership hooks never enter this index.
    const auto actor = late.create(); late.emplace<FormIdComponent>(actor, 31u);
    late.emplace<RemoteComponent>(actor, 7u, 31u, 1u);
    std::atomic_bool done{};
    std::thread reader([&] { while (!done.load()) {
        const auto found = index.RemoteActors.Find(7);
        assert(!found || *found == actor);
    } });
    for (uint32_t i=0; i<10000; ++i)
        late.patch<RemoteComponent>(actor, [i](auto& r) { r.Id = 7 + i % 2; });
    done.store(true); reader.join();
    // Diagnostics observe overlap/reentry without suppressing either scope.
    const auto overlaps = s_dispatchOverlaps.load(), reentries = s_dispatchReentries.load();
    { const EntityDispatchScope nested; }
    std::thread other([] { const EntityDispatchScope concurrent; }); other.join();
    assert(s_dispatchOverlaps.load() == overlaps + 1 && s_dispatchReentries.load() == reentries + 1);
    assert(spdlog::dispatchStacks.load() == 2);
    assert(s_dispatchActive.load() == 1 && s_dispatchDepth == 1 && s_indexOutside.load() > 0);
    // Exhausting read logs must not suppress evidence of an outside writer.
    assert(s_indexOutsideReads.load() > 8 && s_indexOutsideWrites.load() == 0);
    entt::registry loggingRegistry;
    const auto loggingActor = loggingRegistry.create();
    loggingRegistry.emplace<RemoteComponent>(loggingActor, 7u, 31u, 1u);
    EntityViewIndex<RemoteComponent> loggingIndex(loggingRegistry, IdentityKey<RemoteComponent>);
    // The logger must never run under the index lock, including Update's
    // internal erase. A unique-key reader only touches protected containers.
    spdlog::writerLog = [&] {
        std::promise<void> finished;
        auto completion = finished.get_future();
        std::thread loggingReader([&] {
            const auto found = loggingIndex.Find(7);
            assert(!found || *found == loggingActor);
            finished.set_value();
        });
        assert(completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        loggingReader.join();
    };
    std::thread writer([&] {
        for (uint32_t i=0; i<20; ++i)
            loggingRegistry.patch<RemoteComponent>(loggingActor, [i](auto& r) { r.Id=7+i%2; r.OwnershipEpoch=i+1; });
    });
    writer.join();
    spdlog::writerLog = {};
    assert(s_indexOutsideWrites.load() > 8 && spdlog::writerStacks.load() == 8);
}
}

// INSERT PRUNE
void PruneTests() {
    struct Target { void* Body; };
    int a{}, b{}, c{};
    std::unordered_map<void*, int> probes{{&a, 1}, {&b, 2}};
    std::vector<Target> targets{{&a}};
    bool dirty=false;
    PruneFollowProbes(probes, targets, dirty); assert(probes.size()==2);
    dirty=true; PruneFollowProbes(probes, targets, dirty); assert(probes.size()==1 && !dirty);
    probes[&c]=3; targets.push_back({&c}); dirty=true;
    PruneFollowProbes(probes, targets, dirty); assert(probes.size()==2);
    targets.clear(); dirty=true; PruneFollowProbes(probes, targets, dirty); assert(probes.empty());
}

namespace follow_report_tests {
using namespace std::chrono_literals;
// INSERT FOLLOW PROBE
struct State {
    std::unordered_map<void*, FollowProbe> s_followProbes;
    bool s_followProbesDirty{};
    std::chrono::steady_clock::time_point s_nextFollowReport{};
    std::vector<FollowProbe> reports, summaries;
    void Current(std::chrono::steady_clock::time_point now) {
        // INSERT FOLLOW REPORT
    }
    void Before(std::chrono::steady_clock::time_point now) {
        // Ungated loop from the reviewed round6 worktree, not 25fa030d.
        for (auto& [body, probe] : s_followProbes) {
            if (probe.Steps && now >= probe.NextSteerLog && probe.Gap > 10.f) {
                reports.push_back(probe); probe.NextSteerLog=now+2s;
            }
            if (probe.NextLog.time_since_epoch().count()==0) probe.NextLog=now+5s;
            if (now >= probe.NextLog) {
                summaries.push_back(probe); probe.Steps=probe.Teleports=0;
                probe.MaxErrorUnits=0.f; probe.NextLog=now+5s;
            }
        }
    }
};
void Run() {
    State current, before;
    int body{};
    current.s_followProbes[&body]={}; before.s_followProbes[&body]={};
    current.s_followProbesDirty=true;
    const auto equal=[](const auto& a,const auto& b) {
        return a.FormId==b.FormId && a.Gap==b.Gap && a.Steps==b.Steps &&
            a.Teleports==b.Teleports && a.MaxErrorUnits==b.MaxErrorUnits &&
            a.NextLog==b.NextLog && a.NextSteerLog==b.NextSteerLog;
    };
    unsigned scans{};
    for (unsigned frame=0; frame<1000; ++frame) {
        const auto now=std::chrono::steady_clock::time_point{} + 100s + frame*16ms;
        for (auto* s : {&current,&before}) {
            auto& p=s->s_followProbes.at(&body);
            ++p.Steps; p.Gap=frame%137 < 20 ? 20.f : 2.f;
            p.MaxErrorUnits=(std::max)(p.MaxErrorUnits,p.Gap);
            if (p.Gap>10.f) s->s_nextFollowReport=(std::min)(s->s_nextFollowReport,p.NextSteerLog);
            s->reports.clear(); s->summaries.clear();
        }
        scans += current.s_followProbesDirty || now>=current.s_nextFollowReport;
        current.Current(now); before.Before(now); current.s_followProbesDirty=false;
        assert(std::equal(current.reports.begin(),current.reports.end(),before.reports.begin(),before.reports.end(),equal));
        assert(std::equal(current.summaries.begin(),current.summaries.end(),before.summaries.begin(),before.summaries.end(),equal));
        assert(equal(current.s_followProbes.at(&body),before.s_followProbes.at(&body)));
    }
    assert(scans<25);
}
}

namespace discovery_tests {
struct ActorExtension { enum class ReconciliationStage { None, WaitingForDisable, WaitingFor3D }; ReconciliationStage Reconciliation{}; bool IsRemote() const { return false; } };
struct Cell { uint32_t formID{}; bool attached{true}; bool IsAttached() const { return attached; } };
struct Actor;
std::unordered_map<uint32_t, Actor*> forms;
struct TESObjectREFR { uint32_t formID{}; virtual ~TESObjectREFR()=default; static TESObjectREFR* GetByHandle(uint32_t h); virtual void* GetNiNode()=0; };
struct Actor : TESObjectREFR { ActorExtension ext; Cell cell; bool root{true}, deleted{}, suppressed{};
    void* GetNiNode() override { return root ? this : nullptr; } bool IsDeleted() const { return deleted; }
    bool IsDead() const { return false; } ActorExtension* GetExtension() { return &ext; } Cell* GetParentCellEx() { return &cell; } };
TESObjectREFR* TESObjectREFR::GetByHandle(uint32_t h) { return forms.contains(h) ? forms.at(h) : nullptr; }
struct TESForm { static TESObjectREFR* GetById(uint32_t id) { return TESObjectREFR::GetByHandle(id); } };
template<class T> T* Cast(TESObjectREFR* p) { return dynamic_cast<T*>(p); }
struct PlayerCharacter { static inline Actor player; static Actor* Get() { return &player; } };
struct ProcessLists { struct Handles { uint32_t length{}; std::vector<uint32_t> data; uint32_t operator[](size_t i) const { return data[i]; } } highActorHandleArray;
    static inline ProcessLists* instance{}; static ProcessLists* Get() { return instance; } };
struct ActorAddedEvent { uint32_t id; }; struct ActorRemovedEvent { uint32_t id; };
struct Dispatcher { std::vector<uint32_t> pending; std::vector<std::pair<char,uint32_t>> events;
    void enqueue(ActorAddedEvent e) { pending.push_back(e.id); }
    void trigger(ActorRemovedEvent e) { events.emplace_back('r',e.id); }
    template<class T> void update() { for(auto id:pending) events.emplace_back('a',id); pending.clear(); } };
struct Characters { bool IsActorDiscoverySuppressed(uint32_t id) { return forms.contains(id) && forms.at(id)->suppressed; } void ObserveDiscoveredActor(Actor*) {} };
struct Party { bool IsInParty() const { return false; } bool IsLeader() const { return false; } };
struct World { Characters chars; Party party; Characters& GetCharacterService() { return chars; } Party& GetPartyService() { return party; } };
struct DiscoveryService { World m_world; Dispatcher m_dispatcher; Set<uint32_t> m_forms; Map<uint32_t,uint64_t> m_seenForms; uint64_t m_census{}; Vector<uint32_t> m_addedForms, m_removedForms; void VisitForms() noexcept; };
// INSERT DISCOVERY
struct DiscoveryBefore : DiscoveryService { void VisitForms() noexcept; };
// INSERT OLD DISCOVERY
void Run() {
    ProcessLists lists; ProcessLists::instance=&lists;
    Actor a,b; a.formID=1; b.formID=2; PlayerCharacter::player.formID=20;
    forms={{1,&a},{2,&b},{20,&PlayerCharacter::player}};
    DiscoveryService service;
    lists.highActorHandleArray={{3},{1,1,2}}; service.VisitForms();
    assert(service.m_forms.size()==3 && service.m_dispatcher.events.size()==3);
    service.m_dispatcher.events.clear(); service.VisitForms(); assert(service.m_dispatcher.events.empty());
    lists.highActorHandleArray={0,{}}; a.ext.Reconciliation=ActorExtension::ReconciliationStage::WaitingForDisable;
    service.VisitForms(); assert(service.m_forms.count(1) && !service.m_forms.count(2));
    a.ext.Reconciliation=ActorExtension::ReconciliationStage::WaitingFor3D;
    service.VisitForms(); assert(service.m_forms.count(1));
    a.cell.attached=false; service.VisitForms(); assert(!service.m_forms.count(1));
    lists.highActorHandleArray={2,{1,2}}; a.suppressed=true; service.VisitForms();
    assert(!service.m_forms.count(1) && service.m_forms.count(2));
    ProcessLists::instance=nullptr; service.VisitForms(); assert(service.m_forms.count(2));
    // Compare exact event order and membership to 25fa030d, including rehashes,
    // retained reconciliations, suppressed actors and temporarily absent lists.
    DiscoveryService current;
    DiscoveryBefore before;
    std::array<Actor, 150> actors;
    forms.clear(); forms[20]=&PlayerCharacter::player;
    for (uint32_t i=0;i<actors.size();++i) { actors[i].formID=256u*i+100; forms[actors[i].formID]=&actors[i]; }
    uint32_t rng=91;
    for (uint32_t tick=0;tick<1000;++tick) {
        lists.highActorHandleArray.data.clear();
        for (auto& actor:actors) {
            rng=rng*1664525u+1013904223u;
            actor.root=(rng%5)!=0; actor.suppressed=(rng%7)==0; actor.deleted=(rng%13)==0;
            actor.cell.attached=(rng%3)!=0;
            actor.ext.Reconciliation=static_cast<ActorExtension::ReconciliationStage>(rng%3);
            if ((rng%11)<8) lists.highActorHandleArray.data.push_back(actor.formID);
        }
        lists.highActorHandleArray.length=uint32_t(lists.highActorHandleArray.data.size());
        ProcessLists::instance=tick%17 ? &lists : nullptr;
        current.m_dispatcher.events.clear(); before.m_dispatcher.events.clear();
        if (tick == 500) current.m_census = UINT64_MAX; // generation wrap
        current.VisitForms(); before.VisitForms();
        assert(current.m_forms==before.m_forms);
        assert(current.m_dispatcher.events==before.m_dispatcher.events);
    }
}
}

namespace observer_tests {
struct Actor {
    uint32_t flags1{2}, flags{};
    bool dead{},disabled{},deleted{},remote{},player{};
    struct Process { uint32_t movementType{}; } process;
    Process* currentProcess{&process};
    bool IsDead() const { return dead; } bool IsDisabled() const { return disabled; }
    bool IsDeleted() const { return deleted; } bool IsRemote() const { return remote; }
    bool IsPlayer() const { return player; } Actor* GetExtension() { return this; }
};
template<class T> using GameArray = std::vector<T>;
struct ProcessLists { uint8_t padding[0x30]{}; GameArray<uint32_t> handles; };
static_assert(offsetof(ProcessLists,handles)==0x30);
thread_local uint8_t* nativeLists{};
thread_local std::vector<Actor*> nativeActors;
thread_local std::vector<uint32_t> resolved;
struct TESObjectREFR {
    static Actor* GetByHandle(uint32_t handle) { resolved.push_back(handle); return nativeActors.at(handle); }
};
template<class T> T* Cast(Actor* actor) { return static_cast<T*>(actor); }
#define POINTER_SKYRIMSE(Type, name, id) auto name = &nativeLists
// INSERT OBSERVER HEADER
// INSERT OBSERVER SNAPSHOT
// INSERT OLD OBSERVERS
#undef POINTER_SKYRIMSE
void Run() {
    ProcessLists lists;
    nativeLists=reinterpret_cast<uint8_t*>(&lists);
    std::array<Actor,16> actors;
    for(auto& actor:actors) nativeActors.push_back(&actor);
    nativeActors.push_back(nullptr);
    actors[1].dead=true; actors[2].disabled=true; actors[3].deleted=true;
    actors[4].remote=true; actors[5].player=true; actors[6].currentProcess=nullptr;
    actors[7].process.movementType=1; actors[8].flags1=0;
    actors[9].flags1|=0x200; actors[10].flags|=1u<<21;
    for(uint32_t i=0;i<nativeActors.size();++i) lists.handles.push_back(i);
    lists.handles.push_back(0); // Preserve duplicate handles and their order.
    const auto expected=OldObservers();
    const auto expectedCalls=resolved;
    const void* previous{};
    for(int tick=0;tick<1000;++tick) {
        resolved.clear();
        ObserverSnapshot snapshot;
        assert(snapshot.Get()==expected && resolved==expectedCalls);
        if(previous) assert(previous==snapshot.Get().data());
        previous=snapshot.Get().data();
        assert(s_observerScratch.empty());
    }
    assert(s_observerScratch.empty() && s_observerScratch.capacity()>=expected.size());
    {
        ObserverSnapshot outer;
        const auto* data=outer.Get().data();
        lists.handles={11,0,12};
        const auto nestedExpected=OldObservers();
        { ObserverSnapshot inner; assert(inner.Get()==nestedExpected && inner.Get().data()!=data); }
        assert(outer.Get()==expected && outer.Get().data()==data);
        // Other threads use independent capacity and source lists.
        std::thread worker([] { ObserverSnapshot empty; assert(empty.Get().empty()); });
        worker.join();
        assert(outer.Get()==expected);
    }
    { ObserverSnapshot fresh; assert(fresh.Get()==OldObservers()); }
    nativeLists=nullptr;
    { ObserverSnapshot empty; assert(empty.Get().empty()); }
    assert(s_observerScratch.empty());
    nativeActors.clear();
    std::cout << "Observer snapshot: baseline filters/order, 1000 buffer reuses, nesting, thread isolation and no stale actors passed\n";
}
}

namespace awareness_tests {
struct Actor { uint32_t flags1{},flags2{}; bool player{},los{},missing{}; int32_t level{}; Actor* commander{};
    Actor* GetCommandingActor() { return commander; } Actor* GetExtension() { return this; } bool IsPlayer() const { return player; } };
std::vector<Actor*> calls;
const uint8_t* ReadState(Actor* a, Actor*, uint32_t mode) { assert(mode==3); calls.push_back(a); if(a->missing)return nullptr;
    alignas(4) static std::array<uint8_t,32> state{}; *reinterpret_cast<int32_t*>(state.data()+0x10)=a->level; state[0x15]=a->los; return state.data(); }
#define POINTER_SKYRIMSE(Type, name, id) auto name = &ReadState
// INSERT AWARENESS
#undef POINTER_SKYRIMSE
void Run() {
    Actor target, ally, commanded, commander, hidden, seen, absent;
    ally.flags1=1u<<26; commander.player=true; commanded.commander=&commander;
    hidden.level=-15; seen.level=22; seen.los=true; absent.missing=true;
    std::vector<Actor*> observers{&target,&ally,&commanded,&hidden,&absent,&seen};
    for (int i=0;i<10;++i) { calls.clear(); int32_t level{}; uint32_t los{}; GetAwareness(&target,level,los,observers);
        assert(level==22 && los==1 && calls==std::vector<Actor*>({&hidden,&absent,&seen})); }
    int32_t level{}; uint32_t los{}; GetAwareness(&target,level,los,{}); assert(level==-1000 && los==0);
}
}

namespace loot_tests {
using namespace entity_tests;
struct UpdateEvent {};
uint64_t now{100};
uint64_t GetTickCount64() { return now; }
struct NpcWornData { uint32_t ServerId{},OwnershipEpoch{}; uint64_t Sequence{}; };
struct RequestNpcWorn : NpcWornData {};
struct PlayerComponent {}; struct WaitingForAssignmentComponent {};
struct Transport { std::function<bool(const RequestNpcWorn&)> send; bool IsConnected() { return true; } bool Send(const RequestNpcWorn& r) { return send(r); } };
struct Party { uint64_t GetStartEpoch() { return 4; } };
struct World : entt::registry { Transport transport; Party party; Transport& GetTransport() { return transport; } Party& GetPartyService() { return party; } };
struct Actor {
    struct Extension { bool IsPlayer() { return false; } bool IsRemote() { return true; } } extension;
    struct Cell { uint32_t formID{42}; } cell;
    const void* root{this};
    Extension* GetExtension() { return &extension; }
    Cell* GetParentCellEx() { return &cell; }
    const void* GetNiNode() { return root; }
};
struct TESForm {
    static inline std::unordered_map<uint32_t, Actor*> actors;
    static Actor* GetById(uint32_t id) { const auto it=actors.find(id); return it==actors.end() ? nullptr : it->second; }
};
template<class T> T* Cast(Actor* actor) { return static_cast<T*>(actor); }
struct NpcLootService {
    struct State {
        NpcWornData Data; const void* Root{this}; uint32_t Cell{},ObservedEpoch{};
        uint64_t NextCapture{1000},NextQuery{},NextObserve{},LastSeen{};
        bool Pending{},GuardSeeded{};
    };
    struct Target { uint32_t Form, Id, Epoch; bool Local; };
    World m_world; std::recursive_mutex m_lock;
    std::unordered_map<uint32_t, State> m_sent,m_received;
    std::unordered_set<uint32_t> m_dirty;
    Vector<Target> m_targets; Vector<NpcWornData> m_outgoing;
    uint64_t m_session{4},m_nextTargets{1000},m_scan{}; bool m_active{true}; std::atomic_bool m_loaded{};
    std::atomic_bool m_targetsDirty{true};
    std::array<entt::scoped_connection, 15> m_targetConnections;
    void OnTargetChanged(entt::registry&, entt::entity) noexcept;
    NpcLootService() {
        // INSERT NPC TARGET WATCH
    }
    void OnDisconnected(UpdateEvent) { std::lock_guard lock(m_lock); m_sent.clear(); m_received.clear(); m_outgoing.clear(); m_targets.clear(); m_active=false; }
    void OnUpdate(const UpdateEvent&) noexcept;
    static bool IsMainThread() { return true; }
    uint32_t observations{},publications{};
    void OnMainFrame() noexcept;
    void Publish(Actor*, uint32_t, uint32_t, uint64_t) { ++publications; }
    void Observe(Actor*, State& state, uint64_t) { if(state.Pending) { ++observations; state.Pending=false; } }
};
// INSERT NPC TARGET CHANGED
// INSERT NPC UPDATE
// INSERT NPC MAIN FRAME
void Run() {
    NpcLootService service;
    service.m_outgoing={{1,2,3},{2,2,0},{3,2,7}};
    service.m_sent[1].Data={1,2,3}; service.m_sent[3].Data={3,2,7};
    std::vector<uint32_t> order;
    service.m_world.transport.send=[&](const RequestNpcWorn& request) {
        bool unlocked{};
        std::thread worker([&] { unlocked=service.m_lock.try_lock(); if(unlocked) service.m_lock.unlock(); }); worker.join();
        assert(unlocked); order.push_back(request.ServerId);
        // Simulate main-frame work while transport is busy. New work must survive.
        std::lock_guard lock(service.m_lock);
        if(request.ServerId==1) service.m_outgoing.push_back({4,2,1});
        if(request.ServerId==3) ++service.m_sent[3].Data.Sequence;
        return false;
    };
    service.OnUpdate({});
    assert(order==std::vector<uint32_t>({1,2,3}));
    assert(service.m_sent[1].Root==nullptr && service.m_sent[1].NextCapture==0);
    assert(service.m_sent[3].Root!=nullptr && service.m_sent[3].Data.Sequence==8);
    assert(service.m_outgoing.size()==1 && service.m_outgoing.front().ServerId==4);
    service.m_world.transport.send=[](const auto&) { return true; };
    const auto entity = service.m_world.create();
    service.m_world.emplace<FormIdComponent>(entity, 42u);
    service.m_world.emplace<RemoteComponent>(entity, 9u, 42u, 3u);
    auto refresh = [&] { service.m_nextTargets=0; service.OnUpdate({}); };
    refresh(); assert(service.m_targets.size()==1 && !service.m_targetsDirty);
    const auto* storage=service.m_targets.data();
    refresh(); assert(service.m_targets.data()==storage && !service.m_targetsDirty);
    service.m_world.patch<RemoteComponent>(entity, [](auto& r) { ++r.OwnershipEpoch; });
    assert(service.m_targetsDirty); refresh(); assert(service.m_targets[0].Epoch==4);
    service.m_world.emplace<PlayerComponent>(entity); refresh(); assert(service.m_targets.empty());
    service.m_world.remove<PlayerComponent>(entity); refresh(); assert(service.m_targets.size()==1);
    service.m_world.remove<RemoteComponent>(entity);
    service.m_world.emplace<LocalComponent>(entity, 9u, 5u);
    service.m_world.emplace<WaitingForAssignmentComponent>(entity); refresh(); assert(service.m_targets.empty());
    service.m_world.remove<WaitingForAssignmentComponent>(entity); refresh(); assert(service.m_targets[0].Local);
    service.m_world.destroy(entity); refresh(); assert(service.m_targets.empty());

    // A stable ECS target can change native root/cell, and a retry can become
    // due without any component signal. Execute the production polling body.
    Actor native;
    TESForm::actors.emplace(42, &native);
    service.m_targets={{42,9,4,false}};
    auto& received=service.m_received[9];
    received.Data={9,4,1}; received.ObservedEpoch=4; received.Root=native.root;
    received.Cell=native.cell.formID; received.NextQuery=10000;
    service.m_targetsDirty=false; service.m_scan=0;
    service.OnMainFrame(); assert(service.observations==0);
    int replacement{}; native.root=&replacement;
    now=349; service.OnMainFrame(); assert(service.observations==0);
    now=350; service.OnMainFrame(); assert(service.observations==1 && received.Root==&replacement);
    assert(service.m_outgoing.size()==1); // Root replacement resets the query too.
    ++native.cell.formID;
    now=600; service.OnMainFrame(); assert(service.observations==2 && received.Cell==native.cell.formID);
    assert(service.m_outgoing.size()==2);
    assert(!service.m_targetsDirty); // A target-dirty gate would miss both.
    service.m_outgoing.clear();
    received.Data.Sequence=0; received.NextQuery=601;
    now=850; service.OnMainFrame(); assert(service.m_outgoing.size()==1 && received.NextQuery==1850);
    now=1100; service.OnMainFrame(); assert(service.m_outgoing.size()==1);
    now=1850; service.OnMainFrame(); assert(service.m_outgoing.size()==2);
    TESForm::actors.clear();
}
}
int main() {
    PlayerTests(); SendTests(); entity_tests::Run(); PruneTests(); follow_report_tests::Run(); discovery_tests::Run(); observer_tests::Run(); awareness_tests::Run(); loot_tests::Run();
    std::cout << "Scale: player lifecycle; scratch reuse and 5/10-recipient baseline/compression isolation; 10000 ECS differential lifecycle events; discovery; dirty prune; awareness order; unlocked loot sends passed\n";
}
