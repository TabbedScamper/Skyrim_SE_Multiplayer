#include <TiltedOnlinePCH.h>
#include <Services/Generic/NpcLootService.h>
#include <Services/Generic/NakedNpcGuard.h>
#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESObjectCELL.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/EquipmentChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Messages/RequestNpcWorn.h>
#include <Messages/NotifyNpcWorn.h>
#include <Messages/NotifyOwnershipTransfer.h>

namespace
{
std::atomic<DWORD> s_mainThread{};
uint64_t Slots(TESForm* form, bool left)
{
    if (form->formType == FormType::Armor) return Cast<TESObjectARMO>(form)->slotType;
    if (form->formType == FormType::Ammo) return uint64_t{1} << 34;
    if (form->formType == FormType::Weapon || form->formType == FormType::Light)
        return uint64_t{1} << (left ? 33 : 32);
    return 0;
}
std::string Describe(const Vector<NpcWornItem>& items)
{
    std::string result;
    for (const auto& item : items)
        result += fmt::format(" {:X}:{:X}@{:X}", item.Item.BaseId.ModId, item.Item.BaseId.BaseId, item.Slots);
    return result;
}

bool BodyAttached(Actor* actor) noexcept
{
    // Read-only at the existing settled main-thread observation. 40263 returns
    // Actor+268; 15678 uses BIPOBJECT stride 0x78 and clone offset 0x20.
    // A nonnull parent is insufficient: the captured clones belonged to an
    // obsolete root. Follow ancestry to the current root, with a fixed budget.
    static_assert(offsetof(Actor, actorWeightData) == 0x268);
    const auto read = [](const void* source, void* destination, size_t size) {
        SIZE_T bytes{};
        return source && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &bytes) && bytes == size;
    };
    const void* root = actor->GetNiNode();
    const auto* biped = static_cast<const uint8_t*>(actor->actorWeightData);
    const void* clone{};
    constexpr size_t bodyCloneOffset = 0x10 + 2 * 0x78 + 0x20;
    if (!root || !biped || !read(biped + bodyCloneOffset, &clone, sizeof(clone)) || !clone)
        return false;
    const void* node = clone;
    for (uint32_t depth = 0; node && depth < 64; ++depth)
    {
        if (node == root)
        {
            const void* currentClone{};
            return actor->GetNiNode() == root && actor->actorWeightData == biped &&
                read(biped + bodyCloneOffset, &currentClone, sizeof(currentClone)) && currentClone == clone;
        }
        if (!read(static_cast<const uint8_t*>(node) + 0x30, &node, sizeof(node)))
            return false;
    }
    return false;
}
}

void NpcLootService::MarkMainThread() noexcept { s_mainThread.store(GetCurrentThreadId(), std::memory_order_release); }
bool NpcLootService::IsMainThread() noexcept { return s_mainThread.load(std::memory_order_acquire) == GetCurrentThreadId(); }

void NpcLootService::TraceWornModels(Actor* actor, uint32_t slots, const char* phase, uint64_t sequence) noexcept
{
    if (!IsMainThread() || !actor || !((actor->actorState.flags1 >> 21) & 15)) return;
    // Read-only, death transitions only, expected biped slots only. 40263 /
    // 0x14073A760 returns Actor+268. 15659 / 0x140217E30 establishes the
    // 0x10 array and 0x78 stride; 15661 / 0x140218000 uses entry+20 as clone
    // and clone+30 as its parent before detaching it. No tree walk or hook.
    static_assert(offsetof(Actor, actorWeightData) == 0x268);
    struct Part { const void* Item; const void* Addon; const void* Model; const void* Texture; const void* Clone; };
    const auto read = [](const void* source, void* destination, size_t size) {
        SIZE_T bytes{};
        return source && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &bytes) && bytes == size;
    };
    const auto* biped = static_cast<const uint8_t*>(actor->actorWeightData);
    for (uint32_t i = 0; i < 32; ++i)
    {
        if (!(slots & (uint32_t{1} << i))) continue;
        Part part{}; uint32_t item{}; const void* parent{};
        const bool readable = biped && read(biped + 0x10 + i * 0x78, &part, sizeof(part));
        const bool itemReadable = part.Item && read(static_cast<const uint8_t*>(part.Item) + 0x14, &item, sizeof(item));
        const bool parentReadable = part.Clone && read(static_cast<const uint8_t*>(part.Clone) + 0x30, &parent, sizeof(parent));
        spdlog::info("Worn mesh: {:X} {} seq {} slot {} biped {} readable {} item {:X} item-readable {} clone {} parent {} parent-readable {}",
            actor->formID, phase, sequence, i, static_cast<const void*>(biped), readable, item, itemReadable, part.Clone, parent, parentReadable);
    }
}

NpcLootService::NpcLootService(World& world, entt::dispatcher& dispatcher) noexcept
    : m_world(world)
    , m_update(dispatcher.sink<UpdateEvent>().connect<&NpcLootService::OnUpdate>(this))
    , m_worn(dispatcher.sink<NotifyNpcWorn>().connect<&NpcLootService::OnWorn>(this))
    , m_equipment(dispatcher.sink<EquipmentChangeEvent>().connect<&NpcLootService::OnEquipment>(this))
    , m_inventory(dispatcher.sink<InventoryChangeEvent>().connect<&NpcLootService::OnInventory>(this))
    , m_disconnect(dispatcher.sink<DisconnectedEvent>().connect<&NpcLootService::OnDisconnected>(this))
    , m_ownership(dispatcher.sink<NotifyOwnershipTransfer>().connect<&NpcLootService::OnOwnership>(this))
{
    // BEGIN NPC TARGET WATCH
    size_t connection{};
    const auto watch = [&]<class T>() {
        m_targetConnections[connection++] = m_world.on_construct<T>().template connect<&NpcLootService::OnTargetChanged>(this);
        m_targetConnections[connection++] = m_world.on_update<T>().template connect<&NpcLootService::OnTargetChanged>(this);
        m_targetConnections[connection++] = m_world.on_destroy<T>().template connect<&NpcLootService::OnTargetChanged>(this);
    };
    watch.template operator()<FormIdComponent>();
    watch.template operator()<LocalComponent>();
    watch.template operator()<RemoteComponent>();
    watch.template operator()<PlayerComponent>();
    watch.template operator()<WaitingForAssignmentComponent>();
    // END NPC TARGET WATCH
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}
NpcLootService::~NpcLootService() noexcept { EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this); }
BSTEventResult NpcLootService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_loaded.store(true);
    m_world.GetNakedNpcGuard().Reset();
    return BSTEventResult::kOk;
}

bool NpcLootService::Worn(Actor* actor, Vector<NpcWornItem>& result) const noexcept
{
    // Read only instanced worn entries, not the complete base inventory.
    // Missing changes mean unknown, not an authoritative empty outfit.
    const auto* changes = actor->GetContainerChanges();
    if (!changes || !changes->entries) return false;
    for (const auto* entry : *changes->entries)
    {
        if (!entry || !entry->form || !entry->dataList) continue;
        for (auto* extra : *entry->dataList)
        {
            if (!extra) continue;
            Inventory::Entry item;
            TESObjectREFR::GetItemFromExtraData(item, extra);
            if (!item.IsWorn() || !Slots(entry->form, false)) continue;
            if (!m_world.GetModSystem().GetServerModId(entry->form->formID, item.BaseId)) return false;
            item.Count = 1;
            // An extra list can carry both hand flags for identical weapons.
            const bool both = item.ExtraWorn && item.ExtraWornLeft &&
                (entry->form->formType == FormType::Weapon || entry->form->formType == FormType::Light);
            if (both)
            {
                auto right = item; right.ExtraWornLeft = false;
                result.push_back({Slots(entry->form, false), right}); item.ExtraWorn = false;
            }
            result.push_back({Slots(entry->form, item.ExtraWornLeft), item});
        }
    }
    return true;
}

void NpcLootService::Publish(Actor* actor, uint32_t id, uint32_t epoch, uint64_t now) noexcept
{
    if (!actor || actor->GetExtension()->IsPlayer() || actor->GetExtension()->IsRemote() || !epoch) return;
    auto& previous = m_sent[id]; previous.LastSeen = now;
    const auto* cell = actor->GetParentCellEx();
    const uint32_t cellId = cell ? cell->formID : 0;
    const bool changed = previous.Data.OwnershipEpoch != epoch || previous.Root != actor->GetNiNode() || previous.Cell != cellId;
    if (!changed && !m_dirty.contains(actor->formID) && now < previous.NextCapture) return;
    Vector<NpcWornItem> items;
    if (!Worn(actor, items)) return; // Dirty survives incomplete native initialization.
    RequestNpcWorn request;
    request.ServerId = id; request.OwnershipEpoch = epoch;
    request.Sequence = previous.Data.OwnershipEpoch == epoch ? previous.Data.Sequence + 1 : 1;
    request.Items = std::move(items);
    if (!request.Valid()) return;
    if (!changed && NpcSameWorn(previous.Data.Items, request.Items))
    {
        m_dirty.erase(actor->formID); previous.NextCapture = now + 5000 + id % 500;
        return;
    }
    m_outgoing.push_back(request);
    previous.Data = request; previous.Root = actor->GetNiNode(); previous.Cell = cellId;
    previous.NextCapture = now + 5000 + id % 500; m_dirty.erase(actor->formID);
    spdlog::info("Worn evidence: publish {:X} server {} epoch {} seq {} root {} process {} life {} worn [{}]",
        actor->formID, id, epoch, request.Sequence, static_cast<const void*>(actor->GetNiNode()),
        static_cast<const void*>(actor->currentProcess), (actor->actorState.flags1 >> 21) & 15, Describe(request.Items));
    uint32_t slots{};
    for (const auto& item : request.Items) slots |= uint32_t(item.Slots);
    TraceWornModels(actor, slots, "publish", request.Sequence);
}

void NpcLootService::OnWorn(const NotifyNpcWorn& message) noexcept
{
    if (!message.Valid() || !message.Sequence) return;
    std::lock_guard lock(m_lock);
    auto& state = m_received[message.ServerId];
    if (!NpcWornNewer(state.Data, message)) return;
    state.Data = message; state.Pending = true; state.GuardSeeded = false;
    state.NextObserve = 0; state.LastSeen = GetTickCount64();
    // No engine calls or guard mutation from a transport callback.
}

void NpcLootService::Observe(Actor* actor, State& state, uint64_t now) noexcept
{
    if (!state.Pending || now < state.NextObserve) return;
    state.NextObserve = now + 250;
    // These readiness gates retain the pending snapshot for the next main update.
    const uint8_t reason = !actor->GetNiNode() ? 1 : (!actor->currentProcess ? 2 : 0);
    if (reason)
    {
        if (state.WaitReason != reason)
            spdlog::info("Worn evidence: deferred {:X} epoch {} seq {} reason {} (1=root,2=process)",
                actor->formID, state.Data.OwnershipEpoch, state.Data.Sequence, reason);
        state.WaitReason = reason;
        return;
    }
    state.WaitReason = 0;
    Vector<NpcWornItem> actual;
    if (!Worn(actor, actual)) return;
    uint32_t slots{};
    for (const auto& item : state.Data.Items) slots |= uint32_t(item.Slots);
    if (!state.GuardSeeded)
    {
        Inventory intent;
        for (const auto& item : state.Data.Items) intent.Entries.push_back(item.Item);
        m_world.GetNakedNpcGuard().WornSnapshot(state.Data.ServerId, state.Data.OwnershipEpoch, intent);
        state.GuardSeeded = true;
        spdlog::info("Worn evidence: receive {:X} server {} epoch {} seq {} root {} process {} life {} owner [{}] local [{}]",
            actor->formID, state.Data.ServerId, state.Data.OwnershipEpoch, state.Data.Sequence,
            static_cast<const void*>(actor->GetNiNode()), static_cast<const void*>(actor->currentProcess),
            (actor->actorState.flags1 >> 21) & 15, Describe(state.Data.Items), Describe(actual));
        state.NextObserve = now + 3000; // One post-settle observation.
        TraceWornModels(actor, slots, "receive", state.Data.Sequence);
        return;
    }
    spdlog::info("Worn evidence: settled {:X} server {} epoch {} seq {} matches {} root {} process {} life {} owner [{}] local [{}]",
        actor->formID, state.Data.ServerId, state.Data.OwnershipEpoch, state.Data.Sequence,
        NpcSameWorn(state.Data.Items, actual), static_cast<const void*>(actor->GetNiNode()),
        static_cast<const void*>(actor->currentProcess), (actor->actorState.flags1 >> 21) & 15,
        Describe(state.Data.Items), Describe(actual));
    state.Pending = false;
    TraceWornModels(actor, slots, "settled", state.Data.Sequence);
    if (actor->IsTemporary() && state.BodyProofForm != actor->formID)
    {
        spdlog::info("Spawned copy {:X}: body attached {}", actor->formID, BodyAttached(actor));
        state.BodyProofForm = actor->formID;
    }
}

void NpcLootService::OnEquipment(const EquipmentChangeEvent& event) noexcept
{
    if (!m_world.GetTransport().IsConnected()) return;
    std::lock_guard lock(m_lock); m_dirty.insert(event.ActorId);
}
void NpcLootService::OnInventory(const InventoryChangeEvent& event) noexcept
{
    if (!m_world.GetTransport().IsConnected()) return;
    std::lock_guard lock(m_lock); m_dirty.insert(event.FormId);
}
void NpcLootService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    std::lock_guard lock(m_lock);
    m_received.clear(); m_sent.clear(); m_dirty.clear(); m_targets.clear(); m_outgoing.clear();
    m_active = false; m_scan = m_session = m_nextTargets = 0;
    m_targetsDirty.store(true);
}

void NpcLootService::OnTargetChanged(entt::registry&, entt::entity) noexcept
{
    // on_destroy runs before pool removal, so defer the rebuild to the runner.
    m_targetsDirty.store(true, std::memory_order_relaxed);
}

void NpcLootService::OnOwnership(const NotifyOwnershipTransfer& message) noexcept
{
    std::lock_guard lock(m_lock);
    std::erase_if(m_targets, [&](const auto& target) { return target.Id == message.ServerId && target.Epoch < message.OwnershipEpoch; });
    m_nextTargets = m_scan = 0;
    m_targetsDirty.store(true);
}

void NpcLootService::OnUpdate(const UpdateEvent&) noexcept
{
    std::unique_lock lock(m_lock);
    m_active = m_world.GetTransport().IsConnected();
    if (!m_active) return;
    const auto session = m_world.GetPartyService().GetStartEpoch();
    if (m_session && session != m_session) OnDisconnected({});
    m_session = session; m_active = true;
    if (m_loaded.exchange(false))
    {
        m_received.clear(); m_dirty.clear(); m_outgoing.clear(); m_scan = m_nextTargets = 0;
        m_targetsDirty.store(true);
        for (auto& [id, state] : m_sent) { state.Root = nullptr; state.Cell = UINT32_MAX; state.NextCapture = 0; }
    }
    // Registry/transport work stays on the runner. Native work consumes only
    // value tokens on HookMainLoop; no Actor or ExtraDataList pointer is queued.
    const auto now = GetTickCount64();
    if (now >= m_nextTargets)
    {
        m_nextTargets = now + 250;
        if (m_targetsDirty.exchange(false))
        {
            m_targets.clear();
            for (auto entity : m_world.view<FormIdComponent>(entt::exclude<PlayerComponent>))
            {
                const auto* local = m_world.try_get<LocalComponent>(entity);
                const auto* remote = m_world.try_get<RemoteComponent>(entity);
                if ((!local && !remote) || (local && remote)) continue;
                if (local && m_world.any_of<WaitingForAssignmentComponent>(entity)) continue;
                m_targets.push_back({m_world.get<FormIdComponent>(entity).Id,
                    local ? local->Id : remote->Id, local ? local->OwnershipEpoch : remote->OwnershipEpoch, local != nullptr});
            }
        }
    }
    Vector<NpcWornData> outgoing;
    outgoing.swap(m_outgoing);
    lock.unlock();
    for (const auto& data : outgoing)
    {
        RequestNpcWorn request; static_cast<NpcWornData&>(request) = data;
        if (!m_world.GetTransport().Send(request) && data.Sequence)
        {
            std::lock_guard retryLock(m_lock);
            // A main-frame publish or disconnect can occur while sending. Only
            // invalidate the snapshot that failed, never a newer lifetime.
            const auto it = m_sent.find(data.ServerId);
            if (m_active && m_session == session && it != m_sent.end() &&
                it->second.Data.OwnershipEpoch == data.OwnershipEpoch && it->second.Data.Sequence == data.Sequence)
            {
                auto& state = it->second; state.Root = nullptr; state.Cell = UINT32_MAX; state.NextCapture = 0;
            }
        }
    }
    lock.lock();
    if (m_outgoing.empty())
    {
        // Recycle the drained queue's capacity without losing main-frame work
        // that arrived while the transport call was outside the lock.
        outgoing.clear();
        outgoing.swap(m_outgoing);
    }
}

void NpcLootService::OnMainFrame() noexcept
{
    if (!IsMainThread()) return;
    std::lock_guard lock(m_lock);
    if (!m_active || m_loaded.load()) return;
    const auto now = GetTickCount64();
    if (now < m_scan) return;
    m_scan = now + 250;
    // Do not gate this native readiness census on m_targetsDirty. Actor Set3D
    // (1.7.104 ID37178 -> ID19729) replaces/clears the root without an ECS
    // lifecycle signal. The same-epoch root/cell retry below must still run at
    // the existing 250ms boundary even when the registry target list is clean.
    for (const auto& target : m_targets)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(target.Form));
        if (!actor || actor->GetExtension()->IsPlayer() || !target.Epoch) continue;
        if (target.Local)
        {
            Publish(actor, target.Id, target.Epoch, now);
            continue;
        }
        if (!actor->GetExtension()->IsRemote()) continue;
        auto& state = m_received[target.Id]; state.LastSeen = now;
        const auto* cell = actor->GetParentCellEx();
        const uint32_t cellId = cell ? cell->formID : 0;
        if (state.ObservedEpoch != target.Epoch || state.Root != actor->GetNiNode() || state.Cell != cellId)
        {
            state.ObservedEpoch = target.Epoch; state.Root = actor->GetNiNode(); state.Cell = cellId;
            state.NextQuery = state.NextObserve = 0; state.Pending = true; state.GuardSeeded = false;
        }
        if (now >= state.NextQuery && (!state.NextQuery || !state.Data.Sequence || state.Data.OwnershipEpoch != target.Epoch))
        {
            RequestNpcWorn query; query.ServerId = target.Id; query.OwnershipEpoch = target.Epoch;
            m_outgoing.push_back(query); state.NextQuery = now + 1000;
        }
        if (state.Data.OwnershipEpoch == target.Epoch) Observe(actor, state, now);
    }
    std::erase_if(m_received, [&](const auto& entry) { return now - entry.second.LastSeen > 60000; });
    // Unrelated player/container events must not accumulate forever.
    if (m_dirty.size() > 4096) m_dirty.clear();
}
