#include <TiltedOnlinePCH.h>
#include <Services/HarnessService.h>
#include <Services/FarmMode.h>
#include <Services/CorpseRagdollService.h>
#include <World.h>
#include <GameLoopDiagnostic.h>
#include <Events/UpdateEvent.h>
#include <Messages/Harness.h>
#include <Messages/NotifyDoorVote.h>
#include <Games/TES.h>
#include <Games/Memory.h>
#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Forms/TESObjectCELL.h>
#include <Interface/UI.h>
#include <NetImmerse/NiNode.h>
#include <include/cef_parser.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

// Same engine event ABI used by BusyLockService, CommonLib MenuOpenCloseEvent.
struct MenuOpenCloseEvent { BSFixedString menuName; bool opening; uint8_t pad09[7]; };
namespace
{
std::atomic<HarnessService*> s_service{};
std::atomic_bool s_driving{};
uint32_t s_questLookupBudget{}; // Main-thread only, shared by all lookups this tick.
using Dict = CefRefPtr<CefDictionaryValue>;
Dict Parse(const std::string& text)
{
    auto value = CefParseJSON(text, JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY) throw std::runtime_error("expected JSON object");
    return value->GetDictionary()->Copy(false);
}
std::string Json(Dict d)
{
    auto value = CefValue::Create(); value->SetDictionary(d->Copy(false));
    return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}
std::string Str(Dict d, const char* key) { return d->GetString(key).ToString(); }
double Num(Dict d, const char* key, double fallback = 0)
{
    if (!d->HasKey(key)) return fallback;
    const auto type = d->GetType(key);
    if (type != VTYPE_INT && type != VTYPE_DOUBLE) throw std::runtime_error(std::string("expected number: ") + key);
    const double v = type == VTYPE_INT ? d->GetInt(key) : d->GetDouble(key);
    if (!std::isfinite(v)) throw std::runtime_error("nonfinite number");
    return v;
}
uint32_t Id(const std::string& s)
{
    if (s.empty()) return 0;
    size_t used{}; const auto id = std::stoul(s, &used, 16);
    if (used != s.size() || id > UINT32_MAX) throw std::runtime_error("invalid form ID");
    return static_cast<uint32_t>(id);
}
std::filesystem::path Root()
{
    if (FarmMode::Enabled()) return FarmMode::Root();
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn";
}
std::string Read(const std::filesystem::path& path, size_t max = 131072)
{
    if (std::filesystem::file_size(path) > max) throw std::runtime_error("harness file exceeds size bound");
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
TESQuest* Quest(const std::string& name)
{
    // Resolve lazily in bounded slices, including negative lookups. No tick
    // scans the full quest array; forms are re-resolved by ID after discovery.
    struct Entry { std::string Name; uint32_t Cursor{}, Id{}; };
    static std::array<Entry, 16> entries;
    static const void* data{};
    static uint32_t length{};
    auto* mods = ModManager::Get();
    if (!mods) return nullptr;
    if (data != mods->quests.data || length != mods->quests.length)
    {
        entries = {}; data = mods->quests.data; length = mods->quests.length;
    }
    auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.Name == name; });
    if (entry == entries.end())
    {
        entry = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.Name.empty(); });
        if (entry == entries.end() || name.empty()) throw std::runtime_error("quest_lookup_limit_16");
        entry->Name = name;
    }
    if (entry->Id) return Cast<TESQuest>(TESForm::GetById(entry->Id));
    for (unsigned budget = 0; entry->Cursor < length && budget < 32 && s_questLookupBudget; ++budget)
    {
        --s_questLookupBudget;
        if (auto* quest = mods->quests[entry->Cursor++]; quest && name == quest->idName.AsAscii())
        { entry->Id = quest->formID; return quest; }
    }
    return nullptr;
}
bool SameSpace(TESObjectREFR* a, TESObjectREFR* b)
{
    auto* ac = a ? a->GetParentCellEx() : nullptr;
    auto* bc = b ? b->GetParentCellEx() : nullptr;
    return ac && bc && (ac == bc || (ac->worldspace && ac->worldspace == bc->worldspace));
}
bool SameTeleportCell(TESObjectREFR* player, TESObjectREFR* target, const NiPoint3& position)
{
    if (!SameSpace(player, target)) return false;
    const auto* cell = player->GetParentCellEx();
    // Native 19799 derives the physical exterior grid from floored coordinates.
    return !cell->worldspace ||
        (std::floor(player->position.x / 4096.f) == std::floor(position.x / 4096.f) &&
         std::floor(player->position.y / 4096.f) == std::floor(position.y / 4096.f));
}
bool GodMode()
{
    using Get = bool(); POINTER_SKYRIMSE(Get, get, 40501);
    return get.Get()();
}
void SetGodMode(bool enabled)
{
    // 40500 / 14074A4C0 consumes CL, not a member-function this pointer.
    using Set = void(bool); POINTER_SKYRIMSE(Set, set, 40500);
    set.Get()(enabled);
    if (GodMode() != enabled) throw std::runtime_error("god_mode_readback_failed");
}
// Compile through the exact native console script path, never through input.
// 441580/140342A40 ctor, 21883/1403435E0 SetCommand,
// 441582/140343D00 CompileAndRun, 441581/140342AC0 dtor.
void ExecuteNativeConsole(const std::string& command)
{
    if (command.empty() || command.size() > 256 || command.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("console command must be one bounded line");
    if (command.starts_with("player.moveto "))
    {
        auto* target = Cast<TESObjectREFR>(TESForm::GetById(Id(command.substr(14))));
        if (!target || !SameTeleportCell(PlayerCharacter::Get(), target, target->position))
            throw std::runtime_error("moveto requires the same physical cell");
    }
    // Limit the harness's remote command surface to the requested test actions.
    if (command != "tcl" && command != "tgm" && !command.starts_with("player.moveto ") &&
        !command.starts_with("player.setav speedmult ")) throw std::runtime_error("console command not allowed by harness");
    using Ctor = void*(void*); using Set = void(void*, const char*);
    using Run = void(void*, void*, int32_t, TESObjectREFR*); using Dtor = void(void*);
    POINTER_SKYRIMSE(Ctor, ctor, 441580); POINTER_SKYRIMSE(Set, set, 21883);
    POINTER_SKYRIMSE(Run, run, 441582); POINTER_SKYRIMSE(Dtor, dtor, 441581);
    alignas(8) std::array<uint8_t, 0x80> script{};
    uint8_t compiler{};
    ctor.Get()(script.data()); set.Get()(script.data(), command.c_str());
    run.Get()(script.data(), &compiler, 1, nullptr);
    dtor.Get()(script.data());
}
// Disk I/O belongs to the writer. Main-thread capture never waits for disk or
// queue space; overflow is counted and invalidates a successful run.
struct Writer
{
    std::mutex Mutex;
    std::condition_variable Wake;
    // Single producer (game main thread), single consumer (writer thread).
    // Publication counters protect slots; producer never contends on a mutex.
    std::array<std::string, 2048> Queue;
    std::atomic<uint64_t> Consumed{};
    std::thread Thread;
    std::atomic_bool Stop{};
    std::atomic<uint32_t> Dropped{};
    std::atomic_bool Failed{};
    std::atomic<uint64_t> Enqueued{}, Written{};
    std::atomic<std::shared_ptr<const std::string>> PendingStatus{};
    explicit Writer(const std::filesystem::path& path)
    {
        Thread = std::thread([this, path] {
            std::ofstream file(path, std::ios::app | std::ios::binary);
            auto statusPath = path; statusPath.replace_extension("status.jsonl");
            std::ofstream statusFile(statusPath, std::ios::app | std::ios::binary);
            if (!file || !statusFile) Failed = true;
            for (;;)
            {
                if (Consumed.load() == Enqueued.load())
                {
                    std::unique_lock lock(Mutex);
                    Wake.wait_for(lock, 250ms, [&] { return Stop || PendingStatus.load() || Consumed.load() != Enqueued.load(); });
                }
                auto tail = Consumed.load();
                const auto end = Enqueued.load();
                if (Stop && tail == end && !PendingStatus.load()) break;
                for (; tail != end; ++tail)
                {
                    auto line = std::move(Queue[tail % Queue.size()]);
                    Consumed.store(tail + 1);
                    file << line << '\n';
                }
                file.flush();
                if (!file) Failed = true;
                else Written.store(end);
                if (auto status = PendingStatus.exchange(nullptr))
                {
                    // Append-only, so readers never compete with replacement or
                    // deletion. The runner reads the last complete bounded line.
                    statusFile << *status << '\n'; statusFile.flush();
                    if (!statusFile) Failed = true;
                }
            }
        });
    }
    ~Writer()
    {
        Stop = true;
        Wake.notify_one(); if (Thread.joinable()) Thread.join();
    }
    void Append(std::string line)
    {
        const auto head = Enqueued.load();
        if (head - Consumed.load() >= Queue.size()) { ++Dropped; return; }
        Queue[head % Queue.size()] = std::move(line);
        Enqueued.store(head + 1); Wake.notify_one();
    }
};
template <class T> bool ReadAt(uintptr_t base, size_t offset, T& value)
{
    SIZE_T read{};
    return base && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + offset), &value, sizeof(value), &read) && read == sizeof(value);
}
struct CollisionFilterSample { bool Present{}, Readable{}; uint32_t Filter{}; };
CollisionFilterSample SampleCollisionFilter(Actor* actor)
{
    // 39856 / 140722110, controller slot8 implementations 79147/79199.
    // Mirror only their bounded reads, avoiding the rigid getter's world lock.
    CollisionFilterSample result;
    uintptr_t middle{}, controller{}, table{}, getter{}, wrapper{}, object{}, phantom{}, body{};
    if (!actor || !ReadAt(reinterpret_cast<uintptr_t>(actor->currentProcess), 8, middle) ||
        !ReadAt(middle, 0x250, controller) || !controller) return result;
    result.Present = true;
    if (!ReadAt(controller, 0, table) || !ReadAt(table, 8 * sizeof(void*), getter)) return result;
    using Get = uint32_t*(void*, uint32_t*);
    POINTER_SKYRIMSE(Get, proxy, 79147); POINTER_SKYRIMSE(Get, rigid, 79199);
    if (getter == reinterpret_cast<uintptr_t>(proxy.Get()))
        result.Readable = ReadAt(controller, 0x340, wrapper) && ReadAt(wrapper, 0x80, object) &&
            ReadAt(object, 0x18, phantom) && ReadAt(phantom, 0x10, body) && ReadAt(body, 0x4c, result.Filter);
    else if (getter == reinterpret_cast<uintptr_t>(rigid.Get()))
        result.Readable = ReadAt(controller, 0x350, wrapper) && ReadAt(wrapper, 0x20, object) && ReadAt(object, 0x4c, result.Filter);
    return result;
}
NiAVObject* FindNodeBounded(NiAVObject* root, BSFixedString& key, bool& truncated)
{
    // Native GetByName (70299/140EE23A0, leaf70257/140EE0A40) is
    // recursive and unbounded. Mirror its interned-name comparison with a
    // hard 256-object budget and the native array high-water field at +0x122.
    std::array<NiAVObject*, 256> pending{};
    size_t count = root ? 1 : 0;
    size_t edges{};
    pending[0] = root;
    uintptr_t wanted{}; std::memcpy(&wanted, &key, sizeof(wanted));
    for (size_t index = 0; index < count; ++index)
    {
        auto* object = pending[index];
        uintptr_t name{};
        if (!ReadAt(reinterpret_cast<uintptr_t>(object), 0x10, name)) continue;
        if (name == wanted) return object;
        if (auto* node = object->AsNode())
        {
            for (uint16_t child = 0; child < node->children.unk1; ++child)
            {
                if (count == pending.size() || edges++ >= 256) { truncated = true; break; }
                NiAVObject* value{};
                if (ReadAt(reinterpret_cast<uintptr_t>(node->children.data), child * sizeof(void*), value) && value)
                    pending[count++] = value;
                // Null slots still count against traversal work.
                if (child == 255) { truncated = true; break; }
            }
        }
    }
    return nullptr;
}
struct RetainedRoot
{
    NiNode* Value;
    explicit RetainedRoot(NiNode* value) : Value(value) { if (Value) Value->IncRef(); }
    ~RetainedRoot() { if (Value) Value->DecRef(); }
};
}

struct HarnessService::Impl
{
    World& WorldRef;
    std::atomic_bool Enabled{};
    bool Initialized{}, Armor{}, Active{}, Done{}, Entered{}, Submitted{}, Tcl{}, God{}, HadSpeed{}, Recovering{};
    bool Prepared{}, Execute{}, AckSent{}, PreviousGod{}, CollisionOn{};
    uint64_t StepBegan{}, LastTick{}, LoadAllowance{}, ObservedLoadMs{}, LoadingSince{}, NextPrecondition{};
    uint64_t StepTimeout{};
    std::string PreconditionFailure, LastPreconditionFailure;
    uint32_t Recoveries{};
    uint64_t Run{}, Epoch{}, Started{}, Deadline{}, NextRetry{}, LastFrame{}, Frames{}, FinalTicket{}, PendingSince{}, NextStatus{};
    uint32_t Sequence{}, Leader{}, Local{}, LastCaptureUs{}, DoorBits{};
    uint64_t DoorEpoch{}, DoorVote{};
    GameId DoorReference{}, DoorDestination{};
    std::atomic<uint32_t> Dirty{~0u};
    std::atomic_bool InboxOverflow{};
    std::atomic<uint64_t> TriggerToken{};
    std::atomic<uint32_t> TriggerEntrant{0x14};
    uint32_t TriggerGeneration{};
    float PreviousSpeed{100.f};
    NiPoint3 TeleportPosition{};
    uint32_t TeleportCell{};
    std::string State = "idle", Error, Pending, Status = "{\"enabled\":false,\"state\":\"initializing\"}", Stamp, Driver;
    std::string DiskState, DriverKey;
    uint64_t NextPublish{};
    std::string PublishedState, PublishedError;
    uint32_t PublishedSequence{};
    uint32_t DiskSequence{};
    uint32_t StepQuest{};
    Dict Step;
    CefRefPtr<CefListValue> Steps;
    std::vector<uint32_t> Probes, Actors;
    struct AliasWatch { uint32_t Quest{}, Alias{}, Actor = UINT32_MAX; };
    std::vector<AliasWatch> ActorAliases;
    std::array<std::string, 16> ProbeStates;
    size_t ActorCursor{};
    std::array<uint64_t, 3> SceneStates{UINT64_MAX, UINT64_MAX, UINT64_MAX};
    std::array<TESQuest*, 2> Quests{};
    struct Session { bool Online{}, Leader{}; uint64_t Epoch{}; uint32_t LeaderId{}, LocalId{}; size_t Members{}; uint32_t LeaderForm{}; } Current;
    bool LeaderLookup{}; // Mailbox-protected, consumed on the registry update thread.
    std::mutex Mailbox;
    std::deque<HarnessData> Incoming, Outgoing;
    std::unique_ptr<Writer> Log;
    EventDispatcher<MenuOpenCloseEvent>* MenuSource{};
    entt::scoped_connection Update, Message, Door;
    explicit Impl(World& world) : WorldRef(world) {}
    void BindStepCell(Dict step)
    {
        auto pre = step->GetDictionary("preconditions");
        if (pre && pre->GetBool("load_complete") && !pre->HasKey("cell"))
        {
            auto* player = PlayerCharacter::Get(); auto* cell = player ? player->GetParentCellEx() : nullptr;
            if (!cell) throw std::runtime_error("precondition_leader_cell_unavailable");
            pre->SetString("cell", fmt::format("{:X}", cell->formID));
        }
    }
    Dict Preconditions(bool& ready)
    {
        auto d = CefDictionaryValue::Create();
        auto required = Step->GetDictionary("preconditions");
        if (!required) throw std::runtime_error("missing_step_preconditions");
        auto* player = PlayerCharacter::Get();
        auto* cell = player ? player->GetParentCellEx() : nullptr;
        auto* ui = UI::Get();
        const bool loadComplete = player && cell && cell->IsAttached() && player->GetNiNode() && ui &&
            !ui->GetMenuOpen(BSFixedString("Loading Menu"));
        const bool combatClear = player && !player->IsInCombat();
        d->SetBool("loadComplete", loadComplete); d->SetBool("combatClear", combatClear);
        d->SetInt("cell", cell ? cell->formID : 0);
        ready = true; PreconditionFailure.clear();
        const auto require = [&](bool okay, const char* reason) {
            if (!okay) { ready = false; if (PreconditionFailure.empty()) PreconditionFailure = reason; }
        };
        if (required->GetBool("load_complete")) require(loadComplete, "load_incomplete");
        if (required->GetBool("combat_clear")) require(combatClear, "combat_not_clear");
        const auto operation = Str(Step, "op");
        const bool movement = operation == "teleport" || operation == "walk" || operation == "jump" || operation == "follow_objective";
        if (movement)
        {
            // Let the existing cinematic control/collision restoration finish
            // naturally before acknowledging movement readiness.
            // Explicit read-only command bypasses the driver's no-request
            // 100ms throttle, including Execute-time revalidation.
            const auto driver = Parse(WorldRef.GetGameTestService().HarnessDriverTick("{\"command\":\"intro_status\"}"));
            d->SetBool("travelReady", driver->GetBool("travelReady"));
            require(driver->GetBool("travelReady"), "native_travel_not_ready");
            const auto filter = SampleCollisionFilter(player);
            d->SetBool("filterReadable", filter.Readable); d->SetDouble("collisionFilter", filter.Filter);
            if (CollisionOn) require(filter.Readable && !(filter.Filter & (1u << 14)), "controller_collision_not_ready");
        }
        if (required->HasKey("cell")) require(cell && cell->formID == Id(Str(required, "cell")), "cell_mismatch");
        const std::array<const char*, 2> names{"MQ101", "MQ101DragonAttack"};
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (!Quests[i]) Quests[i] = Quest(names[i]);
            auto* quest = Quests[i]; d->SetInt(names[i], quest ? quest->currentStage : -1);
            if (required->HasKey(names[i])) require(quest && quest->currentStage >= Num(required, names[i]), "quest_stage_pending");
        }
        auto scenes = required->GetList("scenes");
        if (scenes && scenes->GetSize() > 16) throw std::runtime_error("precondition_scene_limit");
        bool sceneIdle = true;
        auto observed = CefDictionaryValue::Create();
        if (scenes) for (size_t i = 0; i < scenes->GetSize(); ++i)
        {
            const auto id = scenes->GetString(i).ToString();
            auto* scene = Cast<BGSScene>(TESForm::GetById(Id(id)));
            observed->SetInt(id, scene ? static_cast<int>(scene->isPlaying) : -1);
            if (!scene || scene->isPlaying) sceneIdle = false;
        }
        d->SetBool("sceneIdle", sceneIdle); d->SetDictionary("scenes", observed);
        auto continuing = required->GetList("observed_scenes");
        if (continuing && continuing->GetSize() > 16) throw std::runtime_error("observed_scene_limit");
        auto activeScenes = CefDictionaryValue::Create();
        if (continuing) for (size_t i = 0; i < continuing->GetSize(); ++i)
        {
            const auto id = continuing->GetString(i).ToString();
            auto* scene = Cast<BGSScene>(TESForm::GetById(Id(id)));
            activeScenes->SetInt(id, scene ? static_cast<int>(scene->isPlaying) : -1);
        }
        d->SetDictionary("continuingScenes", activeScenes);
        d->SetDictionary("required", required->Copy(false));
        if (required->GetBool("scene_idle")) require(sceneIdle, "scene_not_idle");
        d->SetString("pending", PreconditionFailure); d->SetBool("ready", ready);
        return d;
    }
    void Record(const std::string& kind, Dict data = {})
    {
        if (!Log) return;
        auto d = data ? data : CefDictionaryValue::Create();
        d->SetString("kind", kind); d->SetString("run", std::to_string(Run));
        d->SetString("epoch", std::to_string(Epoch)); d->SetInt("sequence", Sequence);
        d->SetDouble("wallMs", static_cast<double>(GetTickCount64()));
        d->SetInt("thread", GetCurrentThreadId()); d->SetInt("player", Local);
        Log->Append(Json(d));
    }
    void Send(HarnessOp op, const std::string& payload = {})
    {
        HarnessData data; data.Epoch = Epoch; data.Run = Run; data.Sequence = Sequence;
        data.Op = op; data.Payload = payload.c_str();
        std::lock_guard lock(Mailbox); Outgoing.push_back(std::move(data));
    }
    void Cleanup()
    {
        s_driving = false;
        TriggerToken = 0;
        std::string errors;
        const auto attempt = [&](auto&& action) {
            try { action(); }
            catch (const std::exception& error) { errors += error.what(); errors += "; "; }
        };
        attempt([&] { WorldRef.GetGameTestService().HarnessDriverTick("{\"command\":\"walk_cancel\"}"); });
        if (Tcl) attempt([&] { ExecuteNativeConsole("tcl"); Tcl = false; });
        if (God) attempt([&] { SetGodMode(PreviousGod); God = false; });
        if (HadSpeed) attempt([&] { ExecuteNativeConsole(fmt::format("player.setav speedmult {}", PreviousSpeed)); HadSpeed = false; });
        if (!errors.empty()) throw std::runtime_error(errors);
    }
    void Fail(const std::string& reason, bool send = true)
    {
        Error = reason; State = "failed";
        auto d = CefDictionaryValue::Create(); d->SetString("reason", reason); Record("failed", d);
        if (send && Run && Sequence) Send(HarnessOp::Abort, reason);
        Active = false;
        try { Cleanup(); } catch (const std::exception& error) { Error += "; cleanup: "; Error += error.what(); }
    }
    void Complete()
    {
        if (Done) return;
        Done = true; Record("step_done");
    }
    void Publish()
    {
        // Status is a human/runner observation, not the transport barrier.
        // Rebuild at 4Hz or on a state/step/error change, not every frame.
        static constexpr uint64_t interval = 250;
        const auto now = GetTickCount64();
        if (now < NextPublish && State == PublishedState && Sequence == PublishedSequence && Error == PublishedError) return;
        NextPublish = now + interval; PublishedState = State; PublishedSequence = Sequence; PublishedError = Error;
        auto d = CefDictionaryValue::Create(); d->SetBool("enabled", Enabled); d->SetString("state", State);
        d->SetString("error", Error); d->SetString("run", std::to_string(Run)); d->SetString("stamp", Stamp);
        d->SetString("epoch", std::to_string(Epoch));
        d->SetInt("sequence", Sequence); d->SetBool("active", Active);
        d->SetBool("prepared", Prepared); d->SetBool("executing", Execute);
        d->SetString("precondition", PreconditionFailure);
        d->SetString("metric", CollisionOn ? "collision-on" : "tcl-assisted");
        d->SetString("buildTag", BUILD_BRANCH "@" BUILD_COMMIT);
        d->SetInt("dropped", Log ? Log->Dropped.load() : 0);
        d->SetDouble("elapsedMs", Started ? static_cast<double>(GetTickCount64() - Started) : 0);
        auto status = Json(d);
        if (Log && (GetTickCount64() >= NextStatus || State != DiskState || Sequence != DiskSequence))
        {
            Log->PendingStatus.store(std::make_shared<const std::string>(status)); Log->Wake.notify_one();
            NextStatus = GetTickCount64() + 1000; DiskState = State; DiskSequence = Sequence;
        }
        std::lock_guard lock(Mailbox); Status = std::move(status);
    }
};

HarnessService::HarnessService(World& world) : m(std::make_unique<Impl>(world))
{
    auto& dispatcher = world.GetDispatcher();
    m->Update = dispatcher.sink<UpdateEvent>().connect<&HarnessService::OnUpdate>(this);
    m->Message = dispatcher.sink<NotifyHarness>().connect<&HarnessService::OnMessage>(this);
    m->Door = dispatcher.sink<NotifyDoorVote>().connect<&HarnessService::OnDoor>(this);
    s_service = this;
}
HarnessService::~HarnessService()
{
    s_service = nullptr;
    if (m->Enabled)
    {
        auto* events = EventDispatcherManager::Get();
        events->questStageEvent.UnRegisterSink(this); events->cellFullyLoadedEvent.UnRegisterSink(this);
        events->cellAttachDetachEvent.UnRegisterSink(this); events->sceneEvent.UnRegisterSink(this);
        events->triggerEnterEvent.UnRegisterSink(this); events->packageEvent.UnRegisterSink(this);
        if (m->MenuSource) m->MenuSource->UnRegisterSink(this);
    }
}
bool HarnessService::OwnsDriver() noexcept { return s_driving.load(); }
bool HarnessService::IsEnabled() noexcept
{
    const auto* service = s_service.load();
    return service && service->m->Enabled.load();
}
void HarnessService::MainThreadUpdate() noexcept
{
    auto* service = s_service.load();
    if (!service) return;
    // World exists before CEF. Its JSON API intentionally traps before startup.
    // Overlay publishes this only after CefInitialize succeeds.
    if (!service->m->Initialized)
    {
        if (!service->m->WorldRef.GetOverlayService().IsCefInitialized()) return;
        service->m->Initialized = true;
#ifdef SEAMLESS_HARNESS
        try
        {
            const auto settings = Parse(Read(Root() / "harness.json", 4096));
            service->m->Armor = settings->GetBool("armor");
            service->m->Enabled = settings->GetBool("enabled");
        }
        catch (...) {}
#endif
        if (service->m->Enabled)
        {
            auto* events = EventDispatcherManager::Get();
            events->questStageEvent.RegisterSink(service); events->cellFullyLoadedEvent.RegisterSink(service);
            events->cellAttachDetachEvent.RegisterSink(service); events->sceneEvent.RegisterSink(service);
            events->triggerEnterEvent.RegisterSink(service); events->packageEvent.RegisterSink(service);
        }
        service->m->Publish();
    }
    if (!service->m->Enabled) return;
    try { service->Tick(); }
    catch (const std::exception& error)
    {
        try { service->m->Fail(error.what()); service->m->Publish(); }
        catch (...) { spdlog::error("Harness cleanup failed"); }
    }
}
std::string HarnessService::Command(const std::string& text)
{
    if (!m->WorldRef.GetOverlayService().IsCefInitialized())
        return "{\"enabled\":false,\"state\":\"initializing\"}";
    auto d = Parse(text);
    std::lock_guard lock(m->Mailbox);
    if (Str(d, "command") != "harness_status")
    {
        if (!m->Enabled) throw std::runtime_error("harness is disabled (test build and harness.json required)");
        if (!m->Pending.empty()) throw std::runtime_error("harness command already queued");
        m->Pending = text;
    }
    return m->Status;
}
void HarnessService::OnUpdate(const UpdateEvent&)
{
    if (!m->Enabled) return;
    auto& party = m->WorldRef.GetPartyService();
    auto& transport = m->WorldRef.GetTransport();
    std::deque<HarnessData> outgoing;
    {
        std::lock_guard lock(m->Mailbox);
        uint32_t leaderForm = m->Current.Epoch == party.GetStartEpoch() &&
            m->Current.LeaderId == party.GetLeaderPlayerId() ? m->Current.LeaderForm : 0;
        if (m->LeaderLookup)
        {
            leaderForm = 0;
            auto view = m->WorldRef.view<PlayerComponent, RemoteComponent, FormIdComponent>();
            size_t visited = 0;
            for (auto entity : view)
            {
                if (visited++ == 16) break;
                if (view.get<PlayerComponent>(entity).Id == party.GetLeaderPlayerId())
                {
                    leaderForm = view.get<FormIdComponent>(entity).Id;
                    break;
                }
            }
            m->LeaderLookup = false;
        }
        m->Current = {transport.IsOnline(), party.IsLeader(), party.GetStartEpoch(),
            party.GetLeaderPlayerId(), transport.GetLocalPlayerId(), party.GetPartyMembers().size(), leaderForm};
        outgoing.swap(m->Outgoing);
    }
    for (auto& data : outgoing)
    {
        RequestHarness request; static_cast<HarnessData&>(request) = data;
        transport.Send(request);
    }
}
void HarnessService::OnMessage(const NotifyHarness& data)
{
    if (!m->Enabled || !data.IsValid() || !data.Valid()) return;
    std::lock_guard lock(m->Mailbox);
    if (m->Incoming.size() < 64) m->Incoming.push_back(data);
    else m->InboxOverflow = true;
}
void HarnessService::OnDoor(const NotifyDoorVote& data)
{
    if (!m->Enabled || !data.IsValid()) return;
    // Pass immutable protocol observations into the main-thread capture queue.
    std::lock_guard lock(m->Mailbox);
    if (!data.VoteId || data.Epoch != m->Current.Epoch) return;
    if (data.Action == DoorVoteAction::State && data.VoteId != m->DoorVote)
    {
        m->DoorBits = 0; m->DoorVote = data.VoteId; m->DoorEpoch = data.Epoch;
        m->DoorReference = data.Door; m->DoorDestination = data.Destination;
    }
    if (data.VoteId != m->DoorVote || data.Door != m->DoorReference || data.Destination != m->DoorDestination) return;
    m->DoorBits |= 1u << static_cast<uint8_t>(data.Action);
    m->Dirty.fetch_or(128);
}
#define HARNESS_EVENT(Type, Mask) BSTEventResult HarnessService::OnEvent(const Type*, const EventDispatcher<Type>*) { m->Dirty.fetch_or(Mask); return BSTEventResult::kOk; }
HARNESS_EVENT(TESQuestStageEvent, 1)
HARNESS_EVENT(TESCellFullyLoadedEvent, 2)
HARNESS_EVENT(TESCellAttachDetachEvent, 2)
HARNESS_EVENT(TESSceneEvent, 4)
HARNESS_EVENT(MenuOpenCloseEvent, 8)
HARNESS_EVENT(TESPackageEvent, 32)
#undef HARNESS_EVENT
BSTEventResult HarnessService::OnEvent(const TESTriggerEnterEvent* event, const EventDispatcher<TESTriggerEnterEvent>*)
{
    // Native 26036 / 1404066C0 dispatches {trigger, entrant}, retaining
    // both references across synchronous sinks. Observe only this PC's player.
    auto token = m->TriggerToken.load();
    if (token && event && event->pTrigger && event->pActionRef &&
        event->pTrigger->formID == uint32_t(token) && event->pActionRef->formID == 0x14)
    {
        m->TriggerEntrant = 0x14;
        m->TriggerToken.compare_exchange_strong(token, token | (uint64_t(1) << 63));
    }
    m->Dirty.fetch_or(16);
    return BSTEventResult::kOk;
}
void HarnessService::OnPartyTriggerDelivered(uint32_t aTriggerFormId, uint32_t aRemoteFormId) noexcept
{
    // In play, whichever party member crosses a quest trigger advances the story for everyone.
    auto* service = s_service.load();
    if (!service) return;
    auto token = service->m->TriggerToken.load();
    if (token && !(token & (uint64_t(1) << 63)) && uint32_t(token) == aTriggerFormId)
    {
        service->m->TriggerEntrant = aRemoteFormId;
        service->m->TriggerToken.compare_exchange_strong(token, token | (uint64_t(1) << 63));
    }
    service->m->Dirty.fetch_or(16);
}

void HarnessService::Tick()
{
    auto& s = *m;
    POINTER_SKYRIMSE(uint8_t, alwaysActive, 380768); *alwaysActive = 1;
    // Production's title-menu poll is focus-gated. Unattended test sessions
    // still need its existing Steam/transport queues while neither PC is focused.
    if (auto* ui = UI::Get(); ui && ui->GetMenuOpen(BSFixedString("Main Menu")))
    {
        s.WorldRef.GetSteamLobbyService().PumpCallbacks();
        s.WorldRef.GetTransport().PumpMainMenu();
        OnUpdate(UpdateEvent(0.0));
    }
    if (!s.MenuSource && UI::Get())
    {
        s.MenuSource = reinterpret_cast<EventDispatcher<MenuOpenCloseEvent>*>(reinterpret_cast<uint8_t*>(UI::Get()) + 8);
        s.MenuSource->RegisterSink(this);
    }
    const auto began = std::chrono::steady_clock::now();
    s_questLookupBudget = 64;
    const auto now = GetTickCount64();
    if (s.InboxOverflow.exchange(false)) throw std::runtime_error("inbox_overflow");
    Impl::Session session;
    std::deque<HarnessData> incoming;
    std::string pending;
    uint32_t doorBits{};
    uint64_t doorEpoch{}, doorVote{};
    GameId doorDestination{};
    {
        std::lock_guard lock(s.Mailbox);
        session = s.Current; incoming.swap(s.Incoming); pending.swap(s.Pending);
    }
    if (!pending.empty())
    {
        auto request = Parse(pending);
        if (Str(request, "command") == "harness_stop")
        {
            // Collection cleanup must retain the original failure context.
            if (s.Active || s.State == "starting" || s.State == "waiting_session" || s.State == "flushing")
                s.Fail("stopped by runner");
        }
        else
        {
            if (!s.Active && s.State != "starting" && s.State != "flushing" &&
                (!session.Online || !session.Leader || session.Members < 2 || !session.Epoch))
            {
                if (!s.PendingSince) s.PendingSince = now;
                if (now - s.PendingSince > 90000) throw std::runtime_error("authenticated campaign did not become ready within 90 seconds");
                { std::lock_guard lock(s.Mailbox); if (s.Pending.empty()) s.Pending = pending; }
                s.State = "waiting_session"; s.Publish(); return;
            }
            if (s.Active || s.State == "starting" || s.State == "flushing")
                throw std::runtime_error("start requires healthy authenticated party, campaign epoch and idle leader");
            s.PendingSince = 0;
            const auto name = Str(request, "scenario");
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                throw std::runtime_error("invalid scenario name");
            auto scenario = Parse(Read(Root() / (name + ".json")));
            const auto metric = Str(scenario, "metric");
            if (metric != "collision-on" && metric != "tcl-assisted") throw std::runtime_error("scenario_metric_required");
            auto steps = scenario->GetList("steps");
            s.Steps = steps ? steps->Copy() : nullptr;
            if (!s.Steps || s.Steps->GetSize() == 0 || s.Steps->GetSize() > 256) throw std::runtime_error("scenario needs 1..256 steps");
            for (size_t i = 0; i < s.Steps->GetSize(); ++i)
            {
                if (s.Steps->GetType(i) != VTYPE_DICTIONARY || Json(s.Steps->GetDictionary(i)).size() > HarnessData::MaxPayload)
                    throw std::runtime_error("scenario step is not a bounded object");
                auto item = s.Steps->GetDictionary(i); item->SetString("metric", metric);
                if (!item->GetDictionary("preconditions")) throw std::runtime_error("missing_step_preconditions");
                if (metric == "collision-on" && (Str(item, "cmd") == "tcl" || !Str(item, "fallback").empty()))
                    throw std::runtime_error("collision_on_forbids_tcl");
            }
            s.Stamp = Str(request, "stamp");
            if (s.Stamp.empty() || s.Stamp.find_first_not_of("0123456789-") != std::string::npos) throw std::runtime_error("invalid run stamp");
            s.Run = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            s.Epoch = session.Epoch; s.Leader = session.LeaderId; s.Local = session.LocalId; s.Sequence = 1;
            auto step = s.Steps->GetDictionary(0)->Copy(false); step->SetString("stamp", s.Stamp);
            s.BindStepCell(step);
            s.Send(HarnessOp::Step, Json(step)); s.State = "starting"; s.Started = now; s.Deadline = now + 15000;
        }
    }
    for (const auto& message : incoming)
    {
        if (!session.Online || message.Epoch != session.Epoch || message.Sender != session.LeaderId) continue;
        if (message.Op == HarnessOp::Step)
        {
            if (message.Run == s.Run && !s.Active && s.State != "starting") continue;
            if (s.Active && message.Run == s.Run && message.Sequence == s.Sequence)
            {
                if (s.AckSent) s.Send(HarnessOp::Done);
                continue;
            }
            if (message.Run < s.Run || (message.Run != s.Run && message.Sequence != 1) ||
                (message.Run == s.Run && s.Active && message.Sequence != s.Sequence + 1)) continue;
            auto step = Parse(message.Payload.c_str());
            if (message.Sequence == 1)
            {
                if (s.Active) throw std::runtime_error("overlapping harness runs");
                s.Stamp = Str(step, "stamp");
                if (s.Stamp.empty() || s.Stamp.find_first_not_of("0123456789-") != std::string::npos) throw std::runtime_error("invalid run stamp");
                std::filesystem::create_directories(Root() / "logs");
                s.Log = std::make_unique<Writer>(Root() / "logs" / ("harness-" + s.Stamp + ".jsonl"));
                CorpseRagdollService::BeginRagdollCapture();
                s.Started = now; s.Frames = 0; s.LastFrame = 0; s.LastCaptureUs = 0;
                s.Probes.clear(); s.Actors.clear(); s.ActorAliases.clear();
                s.SceneStates.fill(UINT64_MAX);
                s.ProbeStates.fill({});
                { std::lock_guard lock(s.Mailbox); s.DoorBits = 0; s.DoorVote = 0; }
                s.Error.clear(); s.Tcl = s.God = s.HadSpeed = false;
            }
            s.Run = message.Run; s.Epoch = message.Epoch; s.Sequence = message.Sequence;
            s.Leader = message.Sender; s.Local = session.LocalId;
            s.Step = step; s.Active = true; s.Done = s.Entered = s.Submitted = false;
            if (step->GetBool("party_trigger"))
            {
                auto wait = step->GetDictionary("party_wait");
                if ((Str(step, "op") != "walk" && Str(step, "op") != "teleport") || !Id(Str(step, "until_trigger")) || !wait ||
                    !wait->HasKey("x") || !wait->HasKey("y") || !wait->HasKey("z") ||
                    Num(wait, "radius", 8) < 8 || Num(wait, "radius", 8) > 64)
                    throw std::runtime_error("invalid_party_trigger_wait_target");
                for (const auto* axis : {"x", "y", "z"}) (void)Num(wait, axis);
                if (s.Local != s.Leader)
                {
                    std::lock_guard lock(s.Mailbox);
                    s.Current.LeaderForm = 0;
                    s.LeaderLookup = true;
                }
            }
            s.StepQuest = 0;
            s.Prepared = s.Execute = s.AckSent = false; s.NextPrecondition = 0;
            s.TriggerToken = 0;
            s.PreconditionFailure.clear(); s.LastPreconditionFailure.clear();
            s.CollisionOn = Str(step, "metric") == "collision-on";
            s.Driver.clear(); s.NextRetry = 0; s.State = "running"; s_driving = true;
            s.DriverKey.clear();
            s.Recovering = false; s.Recoveries = 0;
            const auto timeout = Num(step, "timeout", 180);
            if (timeout < 1 || timeout > 590) throw std::runtime_error("timeout must be 1..590 seconds");
            const double partyScale = std::clamp(1.0 + 0.25 * (static_cast<double>(session.Members) - 2.0), 1.0, 3.0);
            s.StepTimeout = static_cast<uint64_t>(timeout * 1000 * partyScale) + std::min<uint64_t>(120000, s.ObservedLoadMs * 2);
            // Both phases reserve time for the slowest participant to load.
            s.Deadline = now + s.StepTimeout + 300000; s.StepBegan = s.LastTick = now; s.LoadAllowance = 300000;
            s.Record("step", step->Copy(false)); s.Dirty = ~0u;
        }
        else if (message.Run == s.Run && message.Sequence == s.Sequence && (s.Active || s.State == "starting"))
        {
            if (message.Op == HarnessOp::Abort) s.Fail(message.Payload.c_str(), false);
            else if (message.Op == HarnessOp::Execute && s.Prepared && !s.Execute)
            {
                bool ready{}; auto observed = s.Preconditions(ready);
                s.Record("precondition_release", observed);
                if (!ready) throw std::runtime_error("precondition_changed: " + s.PreconditionFailure);
                s.Execute = true; s.Deadline = now + s.StepTimeout + 300000;
            }
            else if (message.Op == HarnessOp::Barrier && s.Done)
            {
                s.Record("barrier");
                if (Str(s.Step, "op") == "finish")
                {
                    s.Cleanup();
                    if (session.Leader) s.Send(HarnessOp::Finish);
                    s.Record("actions_complete"); s.FinalTicket = s.Log->Enqueued.load();
                    s.State = "flushing"; s.Active = false;
                }
                else if (session.Leader)
                {
                    if (!s.Steps || s.Sequence >= s.Steps->GetSize()) throw std::runtime_error("scenario must terminate with finish");
                    auto next = s.Steps->GetDictionary(s.Sequence)->Copy(false);
                    s.BindStepCell(next);
                    ++s.Sequence; s.Send(HarnessOp::Step, Json(next));
                    // Keep current sequence until the server echoes the next step.
                    --s.Sequence;
                }
            }
        }
    }
    if (!s.Active)
    {
        if (s.State == "passed" && s.Log && (s.Log->Failed || s.Log->Dropped))
            s.Fail("terminal capture/status write failed");
        if (s.State == "starting" && now > s.Deadline) s.Fail("server did not accept harness start within 15 seconds");
        if (s.State == "flushing")
        {
            if (s.Log->Failed || s.Log->Dropped) s.Fail("final capture write failed");
            else if (s.Log->Written >= s.FinalTicket && !s.Log->Failed.load()) s.State = "passed";
        }
        s.Publish(); return;
    }
    if (!session.Online || session.Epoch != s.Epoch || session.LeaderId != s.Leader) { s.Fail("session lost or authority changed"); s.Publish(); return; }
    if (s.Log->Dropped || s.Log->Failed) { s.Fail("capture writer overflow or I/O failure"); s.Publish(); return; }

    auto* player = PlayerCharacter::Get();
    auto* cell = player ? player->GetParentCellEx() : nullptr;
    // Console toggle 13375 / 1401A7270: byte 400334 is 1 when global
    // collisions are OFF. Selected-reference TCL uses ref flag 0x10.
    POINTER_SKYRIMSE(uint8_t, collisionDisabled, 400334);
    const bool collisionOn = *collisionDisabled.Get() == 0;
    const auto filter = SampleCollisionFilter(player);
    s.Log->Append(fmt::format("{{\"kind\":\"collision\",\"wallMs\":{},\"sequence\":{},\"globalDisabled\":{},\"collisionOn\":{},\"playerPresent\":{},\"playerFlags\":{},\"playerNoCollision\":{},\"controllerPresent\":{},\"filterReadable\":{},\"filter\":{},\"controllerNoCollision\":{},\"nonAtomic\":true}}",
        now, s.Sequence, *collisionDisabled.Get(), collisionOn, player != nullptr, player ? player->flags : 0, player && (player->flags & 0x10) != 0,
        filter.Present, filter.Readable, filter.Filter, (filter.Filter & (1u << 14)) != 0));
    if (s.CollisionOn && (!collisionOn || (player && (player->flags & 0x10))))
        throw std::runtime_error("collision_on_metric_collision_disabled");
    auto* ui = UI::Get();
    const bool loading = !ui || ui->GetMenuOpen(BSFixedString("Loading Menu"));
    if (loading || s.LoadingSince)
    {
        const auto extension = std::min(s.LoadAllowance, now - s.LastTick);
        s.Deadline += extension; s.LoadAllowance -= extension;
    }
    if (loading) { if (!s.LoadingSince) s.LoadingSince = now; }
    else if (s.LoadingSince) { s.ObservedLoadMs = now - s.LoadingSince; s.LoadingSince = 0; }
    s.LastTick = now;
    if (now > s.Deadline)
    {
        s.Fail(s.Execute ? "step_deadline_expired" : s.Prepared ? "peer_precondition_timeout" : "precondition_timeout: " + s.PreconditionFailure);
        s.Publish(); return;
    }
    const auto dirty = s.Dirty.exchange(0);
    {
        // OnDoor publishes fields before its dirty bit under this mutex. Read
        // after consuming dirty so a release event cannot pair with stale bits.
        std::lock_guard lock(s.Mailbox);
        doorBits = s.DoorBits; doorEpoch = s.DoorEpoch;
        doorVote = s.DoorVote; doorDestination = s.DoorDestination;
    }
    const auto op = Str(s.Step, "op");
    if (s.CollisionOn && s.Execute && !s.Done && (op == "walk" || op == "jump" || op == "follow_objective" || op == "teleport") &&
        !loading && (!filter.Readable || (filter.Filter & (1u << 14))))
        throw std::runtime_error(filter.Readable ? "collision_on_controller_disabled" : "collision_on_controller_unknown");
    const bool first = s.Execute && !s.Entered; if (s.Execute) s.Entered = true;
    const bool evaluate = first || dirty;
    if (!s.Prepared && (dirty || now >= s.NextPrecondition))
    {
        bool ready{}; auto observed = s.Preconditions(ready); s.NextPrecondition = now + 250;
        if (!ready && s.PreconditionFailure != s.LastPreconditionFailure)
        {
            s.LastPreconditionFailure = s.PreconditionFailure;
            s.Record("precondition_wait", observed->Copy(false));
        }
        if (ready)
        {
            s.Record("precondition_ack", observed->Copy(false));
            if (s.Log->Dropped || s.Log->Failed) throw std::runtime_error("capture_drop_before_prepared");
            s.Prepared = true; s.Send(HarnessOp::Prepared, Json(observed));
        }
    }
    const auto doorReady = [&](uint32_t destination) {
        const auto required = (1u << static_cast<uint8_t>(DoorVoteAction::State)) |
            (1u << static_cast<uint8_t>(DoorVoteAction::Go)) | (1u << static_cast<uint8_t>(DoorVoteAction::Release));
        GameId expected{};
        return doorVote && doorEpoch == s.Epoch &&
            !(doorBits & (1u << static_cast<uint8_t>(DoorVoteAction::Cancel))) &&
            (doorBits & required) == required &&
            s.WorldRef.GetModSystem().GetServerModId(destination, expected) && expected == doorDestination;
    };
    if (s.Execute && !s.Done)
    {
        if (op == "capture")
        {
            auto d = s.Step->Copy(false); s.Record("capture", d); s.Complete();
        }
        else if (op == "watch")
        {
            auto refs = s.Step->GetList("refs"); auto actors = s.Step->GetList("actors");
            auto aliases = s.Step->GetList("actor_aliases");
            if (refs && s.Probes.size() + refs->GetSize() > 16) throw std::runtime_error("reference watch limit 16");
            if (actors && s.Actors.size() + actors->GetSize() > 16) throw std::runtime_error("actor watch limit 16");
            if (refs) for (size_t i = 0; i < refs->GetSize(); ++i) s.Probes.push_back(Id(refs->GetString(i).ToString()));
            if (actors) for (size_t i = 0; i < actors->GetSize(); ++i) s.Actors.push_back(Id(actors->GetString(i).ToString()));
            if (aliases && s.ActorAliases.size() + aliases->GetSize() > 8) throw std::runtime_error("actor alias watch limit 8");
            if (aliases) for (size_t i = 0; i < aliases->GetSize(); ++i)
            {
                auto alias = aliases->GetDictionary(i);
                if (!alias) throw std::runtime_error("invalid actor alias watch");
                const auto index = Num(alias, "alias", -1);
                if (index < 0 || index > UINT32_MAX || std::floor(index) != index) throw std::runtime_error("invalid alias ID");
                s.ActorAliases.push_back({Id(Str(alias, "quest")), static_cast<uint32_t>(index)});
            }
            s.Complete();
        }
        else if (op == "wait_seconds")
        {
            if (first)
            {
                const auto seconds = Num(s.Step, "seconds");
                if (seconds < 0 || seconds > 580) throw std::runtime_error("seconds must be 0..580");
                s.NextRetry = now + static_cast<uint64_t>(seconds * 1000);
            }
            if (now >= s.NextRetry) s.Complete();
        }
        else if (op == "wait_stage" && (evaluate || !s.StepQuest))
        {
            auto* quest = s.StepQuest ? Cast<TESQuest>(TESForm::GetById(s.StepQuest)) : Quest(Str(s.Step, "quest"));
            if (quest) s.StepQuest = quest->formID;
            const auto stage = Num(s.Step, "stage");
            if (stage < 0 || stage > 65535 || std::floor(stage) != stage)
                throw std::runtime_error("stage must be a uint16");
            if (quest && (s.Step->GetBool("done") ? quest->IsStageDone(static_cast<uint16_t>(stage)) :
                quest->currentStage >= stage)) s.Complete();
        }
        else if (op == "wait_cell" && evaluate)
        {
            const auto destination = Id(Str(s.Step, "cell"));
            if (cell && cell->formID == destination && !loading &&
                (!s.Step->GetBool("door_vote") || (cell->IsAttached() && player->GetNiNode() && doorReady(destination)))) s.Complete();
        }
        else if (op == "wait_menu" && evaluate)
        {
            if (ui && ui->GetMenuOpen(BSFixedString(Str(s.Step, "menu").c_str())) == s.Step->GetBool("open")) s.Complete();
        }
        else if (op == "wait_scene_end" && evaluate)
        {
            auto* scene = Cast<BGSScene>(TESForm::GetById(Id(Str(s.Step, "scene"))));
            if (scene && !scene->isPlaying) s.Complete();
        }
        else if (op == "creator_finish")
        {
            if (!s.Submitted && evaluate && ui && ui->GetMenuOpen(BSFixedString("RaceSex Menu")))
            {
                auto d = CefDictionaryValue::Create(); d->SetString("command", "creator_finish");
                d->SetString("name", session.Leader ? "Host" : "Follower " + std::to_string(session.LocalId));
                s.Driver = s.WorldRef.GetGameTestService().HarnessDriverTick(Json(d)); s.Submitted = true;
                auto state = Parse(s.Driver);
                if (Str(state, "creator").starts_with("failed")) throw std::runtime_error(Str(state, "creator"));
                s.Complete(); // Commit both creator choices before waiting for menu closure.
            }
        }
        else if (op == "console" && first)
        {
            const auto cmd = Str(s.Step, "cmd");
            if (cmd.starts_with("player.setav speedmult ") && !s.HadSpeed && player) s.PreviousSpeed = player->GetActorValue(30);
            if (cmd == "tcl" && s.CollisionOn) throw std::runtime_error("collision_on_forbids_tcl");
            if (cmd == "tgm") throw std::runtime_error("use_native_god_mode_step");
            ExecuteNativeConsole(cmd);
            if (cmd == "tcl") s.Tcl = !s.Tcl;
            if (cmd.starts_with("player.setav speedmult ")) s.HadSpeed = true;
            s.Record("console", s.Step->Copy(false)); s.Complete();
        }
        else if (op == "god_mode" && first)
        {
            if (!s.God) { s.PreviousGod = GodMode(); s.God = true; }
            SetGodMode(s.Step->GetBool("enabled"));
            auto d = CefDictionaryValue::Create(); d->SetBool("enabled", GodMode());
            d->SetBool("previous", s.PreviousGod); s.Record("god_mode", d); s.Complete();
        }
        else if (op == "wait_controls")
        {
            if (evaluate || now >= s.NextRetry)
            {
                auto state = Parse(s.WorldRef.GetGameTestService().HarnessDriverTick());
                s.NextRetry = now + 250;
                if (state->GetBool("travelReady")) s.Complete();
            }
        }
        else if (op == "teleport")
        {
            if (!player || !cell || loading || !player->GetNiNode())
            {
                if (s.Submitted) throw std::runtime_error("teleport unexpectedly lost loaded player");
            }
            else if (!s.Submitted)
            {
                // Trigger steps teleport onto the floor inside the trigger volume ("x","y","z") and complete on the
                // game's own trigger-enter event; the party follower lands on "party_wait" beside it instead.
                const bool intoTrigger = s.Step->HasKey("until_trigger");
                const bool partyFollower = intoTrigger && s.Step->GetBool("party_trigger") && s.Local != s.Leader;
                // An absolute point ("x","y","z" without "ref") stages relative to the player's own cell.
                const bool absolute = !intoTrigger && !s.Step->HasKey("ref") && s.Step->HasKey("x") && s.Step->HasKey("y") && s.Step->HasKey("z");
                auto* ref = absolute ? static_cast<TESObjectREFR*>(player) :
                    Cast<TESObjectREFR>(TESForm::GetById(Id(Str(s.Step, intoTrigger ? "until_trigger" : "ref"))));
                if (!ref) throw std::runtime_error("teleport staging reference unavailable");
                if (absolute)
                {
                    s.TeleportPosition.x = static_cast<float>(Num(s.Step, "x"));
                    s.TeleportPosition.y = static_cast<float>(Num(s.Step, "y"));
                    s.TeleportPosition.z = static_cast<float>(Num(s.Step, "z"));
                    if (glm::length(s.TeleportPosition - player->position) > 8192.f)
                        throw std::runtime_error("absolute teleport point is too far from the player");
                }
                else if (intoTrigger)
                {
                    auto point = partyFollower ? s.Step->GetDictionary("party_wait") : s.Step;
                    if (!point || !point->HasKey("x") || !point->HasKey("y") || !point->HasKey("z"))
                        throw std::runtime_error("trigger teleport needs x, y, z (and party_wait for a party trigger)");
                    s.TeleportPosition.x = static_cast<float>(Num(point, "x"));
                    s.TeleportPosition.y = static_cast<float>(Num(point, "y"));
                    s.TeleportPosition.z = static_cast<float>(Num(point, "z"));
                    if (glm::length(s.TeleportPosition - ref->position) > 2048.f)
                        throw std::runtime_error("trigger teleport point is not near the trigger");
                }
                else
                {
                    const double dx = Num(s.Step, "dx"), dy = Num(s.Step, "dy"), dz = Num(s.Step, "dz");
                    if (std::abs(dx) > 2048 || std::abs(dy) > 2048 || std::abs(dz) > 2048)
                        throw std::runtime_error("teleport staging offset exceeds 2048 units");
                    s.TeleportPosition.x = ref->position.x + static_cast<float>(dx);
                    s.TeleportPosition.y = ref->position.y + static_cast<float>(dy);
                    s.TeleportPosition.z = ref->position.z + static_cast<float>(dz);
                }
                if (!SameTeleportCell(player, ref, s.TeleportPosition))
                    throw std::runtime_error("teleport staging would cross a physical cell boundary");
                s.WorldRef.GetGameTestService().HarnessDriverTick("{\"command\":\"walk_cancel\"}");
                if (intoTrigger && !partyFollower)
                {
                    s.TriggerGeneration = (s.TriggerGeneration + 1) & 0x7fffffff;
                    s.TriggerToken = (uint64_t(s.TriggerGeneration) << 32) | ref->formID;
                }
                s.TeleportCell = cell->formID;
                // Actor slot A9 / 1406770A0 synchronizes position, controller and 3D.
                // Native Havok-moved 19826 uses 19799 for cell/world reconciliation.
                player->SetPosition(s.TeleportPosition, true);
                using Reconcile = void(TESObjectREFR*, TESObjectCELL*, TESWorldSpace*);
                POINTER_SKYRIMSE(Reconcile, reconcile, 19799);
                reconcile.Get()(player, cell->worldspace ? nullptr : cell, cell->worldspace);
                s.Submitted = true; s.NextRetry = now + 250;
                auto d = s.Step->Copy(false); d->SetInt("cell", s.TeleportCell);
                d->SetDouble("x", s.TeleportPosition.x); d->SetDouble("y", s.TeleportPosition.y);
                d->SetDouble("z", s.TeleportPosition.z); s.Record("teleport_submitted", d);
            }
            else
            {
                if (cell->formID != s.TeleportCell) throw std::runtime_error("teleport changed physical cell");
                const auto delta = player->position - s.TeleportPosition;
                const auto token = s.TriggerToken.load();
                if (s.Step->HasKey("until_trigger") && token)
                {
                    // Leader: done once the native trigger event (local, or a partner's delivered trip) fires.
                    if (token & (uint64_t(1) << 63))
                    {
                        auto d = CefDictionaryValue::Create(); d->SetInt("trigger", uint32_t(token));
                        d->SetInt("leaderId", s.Leader); d->SetInt("localId", s.Local);
                        d->SetInt("generation", (token >> 32) & 0x7fffffff);
                        const auto entrant = s.TriggerEntrant.exchange(0x14); d->SetInt("entrant", entrant);
                        d->SetDouble("x", player->position.x); d->SetDouble("y", player->position.y); d->SetDouble("z", player->position.z);
                        s.Record(entrant == 0x14 ? "local_trigger_enter" : "party_trigger_enter", d);
                        s.TriggerToken = 0; s.Complete();
                    }
                }
                else if (s.Step->HasKey("until_trigger") && s.Step->GetBool("party_trigger") && s.Local != s.Leader)
                {
                    // Party follower: landed beside the trigger; same proximity evidence as the walk.
                    auto wait = s.Step->GetDictionary("party_wait");
                    auto* trigger = Cast<TESObjectREFR>(TESForm::GetById(Id(Str(s.Step, "until_trigger"))));
                    const auto targetDistance = glm::length(delta);
                    if (trigger && now >= s.NextRetry && targetDistance <= Num(wait, "radius", 8))
                    {
                        auto evidence = wait->Copy(false);
                        evidence->SetInt("leaderId", s.Leader); evidence->SetInt("localId", s.Local);
                        evidence->SetString("proximityTo", "trigger");
                        evidence->SetInt("trigger", trigger->formID);
                        evidence->SetDouble("targetDistance", targetDistance);
                        evidence->SetDouble("triggerDistance", glm::length(player->position - trigger->position));
                        evidence->SetDouble("actualX", player->position.x); evidence->SetDouble("actualY", player->position.y);
                        evidence->SetDouble("actualZ", player->position.z);
                        evidence->SetDouble("triggerX", trigger->position.x); evidence->SetDouble("triggerY", trigger->position.y);
                        evidence->SetDouble("triggerZ", trigger->position.z);
                        s.Record("party_trigger_proximity_arrived", evidence); s.Complete();
                    }
                }
                else if (now >= s.NextRetry && glm::length(delta) <= 96.f)
                {
                    s.Record("teleport_arrived"); s.Complete();
                }
            }
        }
        else if (op == "activate" && first)
        {
            auto* ref = Cast<TESObjectREFR>(TESForm::GetById(Id(Str(s.Step, "ref"))));
            if (!ref || !SameSpace(player, ref)) throw std::runtime_error("activation reference unavailable");
            // Explicit harness activations must pass the live door policy. The
            // production replay wrapper intentionally bypasses it.
            if (ref->baseForm && ref->baseForm->formType == FormType::Door)
                throw std::runtime_error("walk into load-door volume to exercise the real door vote");
            ref->Activate(player, 0, nullptr, 1, 0); s.Complete();
        }
        else if (op == "walk" || op == "jump" || op == "follow_objective")
        {
            const bool partyFollower = s.Step->GetBool("party_trigger") && s.Local != s.Leader;
            const auto movementRequest = [&]()
            {
                auto request = s.Step->Copy(false);
                if (partyFollower)
                {
                    auto wait = s.Step->GetDictionary("party_wait");
                    request->Remove("ref"); request->Remove("form_id");
                    for (const auto* axis : {"x", "y", "z"}) request->SetDouble(axis, Num(wait, axis));
                    request->SetDouble("radius", Num(wait, "radius", 8));
                }
                return request;
            };
            const auto consumeTrigger = [&]()
            {
                const auto token = s.TriggerToken.load();
                if (s.Done || !s.Submitted || !(token & (uint64_t(1) << 63))) return;
                auto d = CefDictionaryValue::Create(); d->SetInt("trigger", uint32_t(token));
                d->SetInt("leaderId", s.Leader); d->SetInt("localId", s.Local);
                d->SetInt("generation", (token >> 32) & 0x7fffffff);
                if (player) { d->SetDouble("x", player->position.x); d->SetDouble("y", player->position.y); d->SetDouble("z", player->position.z); }
                const auto entrant = s.TriggerEntrant.exchange(0x14);
                d->SetInt("entrant", entrant); s.Record(entrant == 0x14 ? "local_trigger_enter" : "party_trigger_enter", d);
                s.TriggerToken = 0;
                s.WorldRef.GetGameTestService().HarnessDriverTick("{\"command\":\"walk_cancel\"}");
                s.Complete();
            };
            consumeTrigger();
            if (s.Step->HasKey("until_cell") && cell && cell->formID == Id(Str(s.Step, "until_cell")) && !loading)
                s.Complete();
            bool resultAvailable = false;
            if (!s.Done && !s.Submitted && (evaluate || now >= s.NextRetry) && player && !loading)
            {
                auto d = movementRequest(); d->SetString("command", op == "jump" ? "jump_toward" : op == "walk" ? "walk_to" : "follow_objective");
                // Dragon escape is travel while the player is in combat.
                // The driver's normal non-harness combat guard remains intact.
                d->SetString("allow_combat", "true");
                if (d->HasKey("ref")) d->SetString("form_id", Str(d, "ref"));
                if (s.Step->HasKey("until_trigger") && !partyFollower)
                {
                    const auto trigger = Id(Str(s.Step, "until_trigger"));
                    if (!trigger || op != "walk") throw std::runtime_error("until_trigger_requires_walk_and_reference");
                    s.TriggerGeneration = (s.TriggerGeneration + 1) & 0x7fffffff;
                    s.TriggerToken = (uint64_t(s.TriggerGeneration) << 32) | trigger;
                    auto start = CefDictionaryValue::Create(); start->SetInt("trigger", trigger);
                    start->SetInt("generation", s.TriggerGeneration);
                    start->SetDouble("x", player->position.x); start->SetDouble("y", player->position.y); start->SetDouble("z", player->position.z);
                    s.Record("trigger_walk_submission", start);
                }
                s.Driver = s.WorldRef.GetGameTestService().HarnessDriverTick(Json(d));
                auto state = Parse(s.Driver);
                s.Submitted = state->GetBool("active"); s.NextRetry = now + 1000;
                if (!s.Submitted) s.TriggerToken = 0;
                resultAvailable = true;
            }
            else if (!s.Done && s.Submitted)
            {
                if (op == "follow_objective" && evaluate && !s.Recovering && !loading)
                {
                    auto d = s.Step->Copy(false); d->SetString("command", "follow_objective");
                    d->SetString("allow_combat", "true");
                    s.Driver = s.WorldRef.GetGameTestService().HarnessDriverTick(Json(d));
                }
                else s.Driver = s.WorldRef.GetGameTestService().HarnessDriverTick();
                resultAvailable = true;
            }
            consumeTrigger();
            if (resultAvailable && !s.Done)
            {
                auto state = Parse(s.Driver); const auto result = Str(state, "state");
                const auto driverKey = result + "/" + Str(state, "reason") + "/" + std::to_string(Num(state, "targetId")) +
                    "/" + std::to_string(Num(state, "jumpControllerState")) + "/" + std::to_string(Num(state, "jumpRequestedState"));
                if (driverKey != s.DriverKey) { s.Record("driver", state->Copy(false)); s.DriverKey = driverKey; }
                if (s.Recovering)
                {
                    if (!state->GetBool("active")) { s.Recovering = s.Submitted = false; s.NextRetry = now + 500; }
                }
                else if (op == "jump" && (result == "jump_landed" || result == "jump_finished"))
                {
                    if (Num(state, "distance", 1e9) <= Num(s.Step, "radius", 96)) s.Complete();
                    else throw std::runtime_error("jump_did_not_reach_target: " + s.Driver);
                }
                else if (result == "arrived")
                {
                    if (partyFollower)
                    {
                        auto* trigger = Cast<TESObjectREFR>(TESForm::GetById(Id(Str(s.Step, "until_trigger"))));
                        if (!SameSpace(player, trigger)) throw std::runtime_error("party_trigger_space_mismatch");
                        const auto triggerDistance = glm::length(player->position - trigger->position);
                        auto wait = s.Step->GetDictionary("party_wait");
                        const auto targetDistance = Num(state, "distance", 1e9);
                        if (!std::isfinite(triggerDistance) || targetDistance > Num(wait, "radius", 8))
                            throw std::runtime_error("party_trigger_proximity_not_reached");
                        auto evidence = wait->Copy(false);
                        evidence->SetInt("leaderId", s.Leader); evidence->SetInt("localId", s.Local);
                        evidence->SetString("proximityTo", "trigger");
                        if (triggerDistance > 400.f)
                        {
                            auto* leader = Cast<Actor>(TESForm::GetById(session.LeaderForm));
                            if (!SameSpace(player, leader) || !leader->GetNiNode()) throw std::runtime_error("party_trigger_leader_unavailable");
                            const auto leaderDistance = glm::length(player->position - leader->position);
                            if (!std::isfinite(leaderDistance) || leaderDistance > 600.f)
                                throw std::runtime_error("party_trigger_leader_not_nearby");
                            evidence->SetString("proximityTo", "leader");
                            evidence->SetInt("leaderId", s.Leader);
                            evidence->SetInt("leaderForm", leader->formID);
                            evidence->SetDouble("leaderDistance", leaderDistance);
                            evidence->SetDouble("leaderX", leader->position.x);
                            evidence->SetDouble("leaderY", leader->position.y);
                            evidence->SetDouble("leaderZ", leader->position.z);
                        }
                        evidence->SetInt("trigger", trigger->formID);
                        evidence->SetDouble("targetDistance", targetDistance);
                        evidence->SetDouble("triggerDistance", triggerDistance);
                        evidence->SetDouble("actualX", player->position.x);
                        evidence->SetDouble("actualY", player->position.y);
                        evidence->SetDouble("actualZ", player->position.z);
                        evidence->SetDouble("triggerX", trigger->position.x);
                        evidence->SetDouble("triggerY", trigger->position.y);
                        evidence->SetDouble("triggerZ", trigger->position.z);
                        s.Record("party_trigger_proximity_arrived", evidence);
                    }
                    else if (s.Step->HasKey("until_trigger")) throw std::runtime_error("walk_arrived_without_local_trigger_entry");
                    if (s.Step->HasKey("until_cell")) { s.Submitted = false; s.NextRetry = now + 1000; }
                    else s.Complete();
                }
                else if (result == "transitioned" || result == "interrupted" || result == "waiting")
                {
                    if (s.Step->HasKey("until_cell") && cell && cell->formID == Id(Str(s.Step, "until_cell"))) s.Complete();
                    else { s.Submitted = false; s.NextRetry = now + 1000; }
                }
                else if (result == "failed")
                {
                    if (op == "follow_objective" && Str(state, "reason") == "target unloaded or changed space")
                    {
                        s.Record("objective_leg_ended", state->Copy(false));
                        s.Submitted = false; s.NextRetry = now + 1000;
                    }
                    else if (s.Step->GetBool("step_over") && s.Recoveries < 3 && Num(state, "distance") < 1200)
                    {
                        s.Record("path_stall_recovery", state->Copy(false));
                        auto request = movementRequest(); request->SetString("command", "walk_nudge");
                        if (request->HasKey("ref")) request->SetString("form_id", Str(request, "ref"));
                        else if (!request->HasKey("x"))
                            request->SetString("form_id", fmt::format("{:X}", static_cast<uint32_t>(Num(state, "targetId"))));
                        s.Driver = s.WorldRef.GetGameTestService().HarnessDriverTick(Json(request));
                        s.Submitted = Parse(s.Driver)->GetBool("active"); s.Recovering = s.Submitted; ++s.Recoveries;
                        if (!s.Submitted) throw std::runtime_error("native walking recovery refused: " + s.Driver);
                    }
                    else if (Str(s.Step, "fallback") == "tower_jump" && !s.Tcl)
                    {
                        if (s.CollisionOn) throw std::runtime_error("collision_on_forbids_tcl_fallback");
                        s.Record("tower_path_failed", state);
                        const auto marker = Str(s.Step, "ref");
                        if (marker.empty()) throw std::runtime_error("tower fallback needs a local reference");
                        ExecuteNativeConsole("tcl"); s.Tcl = true;
                        ExecuteNativeConsole("player.moveto " + marker);
                        ExecuteNativeConsole("tcl"); s.Tcl = false;
                        s.Record("tower_tcl_bypass"); s.Complete();
                    }
                    else throw std::runtime_error("native path: " + Str(state, "reason"));
                }
            }
        }
        else if (op == "assert" || op == "finish")
        {
            if (s.Step->HasKey("cell") && (!cell || cell->formID != Id(Str(s.Step, "cell")))) throw std::runtime_error("cell assertion failed");
            if (s.Step->GetBool("door_vote"))
            {
                if (!cell || !doorReady(cell->formID)) throw std::runtime_error("matching real door vote Go/Release not observed");
            }
            s.Complete();
        }
        else if (first && op != "wait_stage" && op != "wait_cell" && op != "wait_menu" && op != "wait_scene_end")
            throw std::runtime_error("unknown step: " + op);
    }

    // Fixed reference count and one armor actor per frame. Bounded named lookup,
    // screenshots, cell sweeps, VM snapshots or logger flush on this thread.
    const auto loop = GetGameLoopDiagnostic();
    std::string refs = "[";
    const std::array<uint32_t, 4> carts{0xB9DF3, 0xBB970, 0xB9DF2, 0xBB971};
    auto sample = [&](uint32_t id) {
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        if (refs.size() > 1) refs += ',';
        if (!ref) { refs += fmt::format("{{\"id\":{},\"exists\":false,\"loaded\":false}}", id); return; }
        refs += fmt::format("{{\"id\":{},\"exists\":true,\"loaded\":{},\"p\":[{},{},{}],\"flags\":{}}}", id, ref->GetNiNode() != nullptr, ref->position.x, ref->position.y, ref->position.z, ref->flags);
    };
    for (auto id : carts) sample(id);
    refs += ']';
    s.Log->Append(fmt::format("{{\"kind\":\"frame\",\"run\":\"{}\",\"sequence\":{},\"wallMs\":{},\"tick\":{},\"frame\":{},\"gapMs\":{},\"vmAppUs\":{},\"vmNativeUs\":{},\"worldGapUs\":{},\"previousTickCaptureUs\":{},\"cell\":{},\"player\":[{},{},{}],\"refs\":{}}}",
        s.Run, s.Sequence, now, s.WorldRef.GetTick(), ++s.Frames, s.LastFrame ? now - s.LastFrame : 0,
        loop.VmLastAppUs, loop.VmLastOriginalUs, loop.WorldLastEntryGapUs, s.LastCaptureUs, cell ? cell->formID : 0,
        player ? player->position.x : 0, player ? player->position.y : 0, player ? player->position.z : 0, refs));
    s.LastFrame = now;
    // At most one 64-body solver observation per harness frame. The solver
    // publishes POD; formatting/IO never run on physics workers.
    if (auto ragdoll = CorpseRagdollService::DrainRagdollCapture(); !ragdoll.empty())
        s.Log->Append(std::move(ragdoll));
    // Fixed, read-only scene telemetry. Native SaveGame 1403A8C60 saves
    // B0 and BC as this state; no phase writes or script/action execution.
    const std::array<uint32_t, 3> scenes{0xBECD4, 0xCD68E, 0xD0594};
    for (size_t index = 0; index < scenes.size(); ++index)
        if (auto* scene = Cast<BGSScene>(TESForm::GetById(scenes[index])))
        {
            const auto state = (uint64_t(scene->isPlaying) << 32) | scene->rawPhaseWord;
            if (state != s.SceneStates[index])
            {
                s.SceneStates[index] = state;
                s.Log->Append(fmt::format("{{\"kind\":\"scene\",\"wallMs\":{},\"id\":{},\"playing\":{},\"rawPhase\":{},\"nonAtomic\":true}}",
                    now, scenes[index], scene->isPlaying, scene->rawPhaseWord));
            }
        }
    if (dirty || first)
    {
        auto d = CefDictionaryValue::Create(); d->SetInt("events", dirty); d->SetInt("doorBits", doorBits);
        const std::array<const char*, 2> names{"MQ101", "MQ101DragonAttack"};
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (!s.Quests[i]) s.Quests[i] = Quest(names[i]);
            if (auto* quest = s.Quests[i]) d->SetInt(names[i], quest->currentStage);
        }
        s.Record("events", d);
    }
    if (!s.Probes.empty())
    {
        const auto probeIndex = s.Frames % s.Probes.size();
        const auto id = s.Probes[probeIndex];
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        RetainedRoot root(ref ? ref->GetNiNode() : nullptr);
        const auto referenceState = ref ? fmt::format("\"id\":{},\"exists\":true,\"loaded\":{},\"flags\":{},\"p\":[{},{},{}],\"rootFlags\":{}",
            id, root.Value != nullptr, ref->flags, ref->position.x, ref->position.y, ref->position.z, root.Value ? root.Value->flags : 0) :
            fmt::format("\"id\":{},\"exists\":false,\"loaded\":false", id);
        if (referenceState != s.ProbeStates[probeIndex])
        {
            s.ProbeStates[probeIndex] = referenceState;
            s.Log->Append(fmt::format("{{\"kind\":\"reference\",\"wallMs\":{},{},\"nonAtomic\":true}}", now, referenceState));
        }
        if (root.Value)
        {
            // Twelve named chunks, one per tick; bounded lookup in this tiny
            // scenario reference only. No global world-state survey.
            const auto name = fmt::format("Chunk{:02}", (s.Frames / s.Probes.size()) % 12 + 1);
            BSFixedString key(name.c_str()); bool truncated{};
            auto* node = FindNodeBounded(root.Value, key, truncated);
            if (node) s.Log->Append(fmt::format("{{\"kind\":\"probe\",\"wallMs\":{},\"id\":{},\"node\":\"{}\",\"p\":[{},{},{}],\"flags\":{},\"collision\":{},\"truncated\":{},\"nonAtomic\":true}}", now, id, name,
                node->world.translate.x, node->world.translate.y, node->world.translate.z, node->flags, node->collisionObject != nullptr, truncated));
            else s.Log->Append(fmt::format("{{\"kind\":\"probe\",\"wallMs\":{},\"id\":{},\"node\":\"{}\",\"missing\":true,\"truncated\":{}}}", now, id, name, truncated));
        }
    }
    if (s.Armor && (!s.Actors.empty() || !s.ActorAliases.empty()))
    {
        // One native alias lookup per frame discovers scenario actors whose
        // runtime clone IDs differ on each PC, without an actor census.
        if (!s.ActorAliases.empty())
        {
            auto& watch = s.ActorAliases[s.Frames % s.ActorAliases.size()];
            auto* quest = Cast<TESQuest>(TESForm::GetById(watch.Quest));
            auto* ref = quest ? quest->GetAliasedRef(watch.Alias) : nullptr;
            const auto id = ref ? ref->formID : 0;
            if (id != watch.Actor)
            {
                watch.Actor = id;
                s.Log->Append(fmt::format("{{\"kind\":\"actor_alias\",\"wallMs\":{},\"quest\":{},\"alias\":{},\"id\":{}}}", now, watch.Quest, watch.Alias, id));
            }
        }
        std::array<uint32_t, 40> actorIds{};
        size_t actorCount{};
        for (auto id : s.Actors) actorIds[actorCount++] = id;
        for (const auto& alias : s.ActorAliases)
            if (alias.Actor && alias.Actor != UINT32_MAX) actorIds[actorCount++] = alias.Actor;
        // Player copies have runtime form IDs; discover current party actors
        // through the existing component mapping, bounded to 16 extra players.
        auto view = s.WorldRef.view<FormIdComponent, PlayerComponent>();
        size_t playersVisited{};
        for (auto entity : view)
        {
            if (actorCount == actorIds.size() || playersVisited++ == 16) break;
            const auto id = view.get<FormIdComponent>(entity).Id;
            actorIds[actorCount++] = id;
        }
        // Fixed stack storage, O(P log P) and no allocation; duplicate watchers
        // must not multiply the armor sampling share of an actor.
        std::sort(actorIds.begin(), actorIds.begin() + actorCount);
        actorCount = std::unique(actorIds.begin(), actorIds.begin() + actorCount) - actorIds.begin();
        const auto id = actorCount ? actorIds[s.ActorCursor++ % actorCount] : 0;
        auto* actor = Cast<Actor>(TESForm::GetById(id));
        if (actor && actor->GetNiNode())
        {
            // One already-scheduled actor, at most 128 rendered bones. Keep
            // this boundary separate from solver body order and target errors.
            if (auto pose = CorpseRagdollService::DescribeRagdollRenderPose(actor); !pose.empty())
                s.Log->Append(std::move(pose));
            RetainedRoot root(actor->GetNiNode());
            const auto biped = reinterpret_cast<uintptr_t>(actor->actorWeightData);
            uintptr_t clone{}, parent{}, item{}; uint8_t skinned{};
            const auto slot = biped + 0x10 + 2 * 0x78;
            const bool read = ReadAt(slot, 0, item) && ReadAt(slot, 0x20, clone) && ReadAt(slot, 0x68, skinned);
            const bool parentRead = ReadAt(clone, 0x30, parent);
            bool attached{}; auto cursor = clone;
            for (unsigned depth = 0; cursor && depth < 16; ++depth)
            {
                if (cursor == reinterpret_cast<uintptr_t>(root.Value)) { attached = true; break; }
                if (!ReadAt(cursor, 0x30, cursor)) break;
            }
            s.Log->Append(fmt::format("{{\"kind\":\"armor\",\"wallMs\":{},\"id\":{},\"readable\":{},\"item\":{},\"clone\":{},\"skinned\":{},\"parentReadable\":{},\"parent\":{},\"attachedWithin16\":{},\"p\":[{},{},{}],\"stateFlags\":{},\"nonAtomic\":true}}", now, id, read, item, clone, skinned != 0, parentRead, parent, attached,
                actor->position.x, actor->position.y, actor->position.z, actor->actorState.flags1));
            if (s.Frames % 29 == 0)
            {
                std::string bones = "[";
                bool truncated{};
                for (const char* name : {"NPC Pelvis [Pelv]", "NPC L Hand [LHnd]", "NPC R Hand [RHnd]"})
                {
                    BSFixedString key(name); auto* bone = FindNodeBounded(root.Value, key, truncated);
                    if (bones.size() > 1) bones += ',';
                    if (bone) bones += fmt::format("[{}, {}, {}]", bone->world.translate.x, bone->world.translate.y, bone->world.translate.z);
                    else bones += "null";
                }
                bones += ']';
                // Sparse vanilla package prerequisite, never changed here.
                // ActorValueOwner getter 1406C4CB0; MQ101CartRiderScript sets
                // Variable01 (68) on ExitCartEnd before EvaluatePackage.
                const auto variable = id == 0x654FB ? fmt::format("{}", actor->GetActorValue(68)) : "null";
                s.Log->Append(fmt::format("{{\"kind\":\"pose\",\"wallMs\":{},\"id\":{},\"stateFlags\":{},\"root\":{},\"pelvisLeftRight\":{},\"variable01\":{},\"truncated\":{},\"nonAtomic\":true}}",
                    now, id, actor->actorState.flags1, reinterpret_cast<uintptr_t>(root.Value), bones, variable, truncated));
            }
        }
    }
    if (s.Log->Dropped || s.Log->Failed) throw std::runtime_error("capture_drop_or_io_failure");
    if (s.Done && !s.AckSent) { s.AckSent = true; s.Send(HarnessOp::Done); }
    s.Publish();
    s.LastCaptureUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
}
