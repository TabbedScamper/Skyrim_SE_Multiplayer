#include <TiltedOnlinePCH.h>
#include <chrono>
#include <Services/Generic/NakedNpcGuard.h>
#include <Services/Generic/NpcLootService.h>

#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <EquipManager.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESBoundObject.h>
#include <Games/ActorExtension.h>
#include <Games/Overrides.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <Components/TESContainer.h>
#include <Services/InventoryService.h>
#include <Services/ActorValueService.h>
#include <Messages/NotifyNpcWorn.h>
#include <Messages/NpcInventory.h>
#include <Events/DisconnectedEvent.h>
#include <Events/EquipmentChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyInventoryChanges.h>

namespace
{
struct GuardCost
{
    std::chrono::steady_clock::time_point Started{HostFrameCost::Begin()};
    ~GuardCost() { HostFrameCost::End(4, Started); }
};
// Research/credit (handoff for docs/REFERENCE_RESEARCH.md):
// arifkulpu/Invisible-and-naked-NPC-fixer, src/VisibilityFixer.cpp:298-332:
// adopt missing-slot detection and existing-inventory equip; reject its reset fallback.
// MrMartexX/TiltedEvolution PR #10 removes RunNakedNPCBugChecks/ResetInventory(false):
// an outfit is NOT evidence that an item still exists (looting must remain permanent).
// rfortier/TiltedEvolution-rwf, InventoryService.cpp:296-367, Actor.cpp:532-572,
// and tiltedphoques/TiltedEvolution PR #860: adopt delayed, non-destructive equip and
// assignment fencing. Extend it to remote copies and corpses using owner worn intent.
// Naked Dead NPC Fix (Nexus SE 99024, ThirdEyeSqueegee/wSkeever): original
// ThirdEyeSqueegee/SkeeverModTwo is now unavailable; public fork clayne/SkeeverModTwo,
// src/Hooks.cpp:11-72, hooks Character::Load3D and silently equips existing body,
// hands, feet and head armor. Adopt the existing-item/corpse policy, but check each
// missing slot independently and require owner intent instead of arbitrary loot.
// Native 1.7.104: 38894/0x1406DC310 -> 38929/0x1406DE920 -> 38001/0x1406B2570
// checks inventory count; 38004/0x1406B3650 removes conflicting biped slots. Therefore
// supply missing owner stock before silent equip. Never reset the inventory.
// Slot 32 (body) is bit 2 of TESObjectARMO::slotType; the same policy covers all armor.
constexpr uint64_t kCheckIntervalMs = 1000;
constexpr uint64_t kSettleMs = 1750; // Quiet notifications and an unchanged loaded root.

constexpr int32_t WornDeliveryCount(int32_t delta, int64_t ownerCount, int64_t localCount) noexcept
{
    return int32_t(std::min<int64_t>(delta, std::max<int64_t>(0, ownerCount - localCount)));
}
// Delayed delivery after provisioning must not duplicate stock; a genuinely
// new spare still arrives. Checked by MSVC /Zs, with no executable test run.
static_assert(WornDeliveryCount(1, 1, 1) == 0);
static_assert(WornDeliveryCount(1, 2, 1) == 1);
static_assert(WornDeliveryCount(3, 3, 1) == 2);
static_assert(WornDeliveryCount(1, 1, 0) == 1);

TESObjectARMO* Armor(World& aWorld, GameId aId) noexcept
{
    auto* pForm = TESForm::GetById(aWorld.GetModSystem().GetGameId(aId));
    return pForm && pForm->formType == FormType::Armor ? Cast<TESObjectARMO>(pForm) : nullptr;
}

bool ReadArmor(World& aWorld, Actor* apActor, Inventory& aInventory) noexcept
{
    const auto* pChanges = apActor->GetContainerChanges();
    if (!pChanges || !pChanges->entries)
        return false;
    // A plain stack/removal can have no extra-list container. GetArmor assumes
    // one exists; rejecting the entire actor can hide repairs after an arrow hit.
    // Keep base counts and signed deltas, including uninstanced armor stacks.
    const auto add = [&](TESForm* apForm, int32_t aCount, ExtraDataList* apExtra) -> int32_t
    {
        if (!apForm || apForm->formType != FormType::Armor)
            return 0;
        Inventory::Entry item;
        if (!aWorld.GetModSystem().GetServerModId(apForm->formID, item.BaseId))
            return 0;
        item.Count = aCount;
        if (apExtra)
            TESObjectREFR::GetItemFromExtraData(item, apExtra);
        const auto count = item.Count;
        // Cancel plain base stock against its deltas before selecting instances.
        // Otherwise a tempered base item looks like two different candidates.
        if (!item.ContainsExtraData())
            for (auto& entry : aInventory.Entries)
                if (entry.BaseId == item.BaseId && !entry.ContainsExtraData())
                {
                    entry.Count += count;
                    return count;
                }
        aInventory.Entries.push_back(std::move(item));
        return count;
    };
    if (const auto* pBase = apActor->GetContainer())
        for (uint32_t i = 0; i < pBase->count; ++i)
            if (const auto* pEntry = pBase->entries[i])
                add(pEntry->form, pEntry->count, nullptr);
    for (const auto* pEntry : *pChanges->entries)
    {
        if (!pEntry)
            continue;
        if (!pEntry->form)
            continue;
        if (pEntry->form->formType != FormType::Armor)
            continue;
        int32_t remainder = pEntry->count;
        if (pEntry->dataList)
            for (auto* pExtra : *pEntry->dataList)
                if (pExtra)
                {
                    remainder -= add(pEntry->form, 1, pExtra);
                }
        if (remainder)
            add(pEntry->form, remainder, nullptr);
    }
    aInventory.RemoveByFilter([](const auto& entry) { return entry.Count == 0; });
    return true;
}

bool SameInstance(Inventory::Entry aLeft, Inventory::Entry aRight) noexcept
{
    return NpcSameItem(aLeft, aRight, true);
}

Inventory::Entry Unworn(Inventory::Entry item) noexcept
{
    item.Count = 1;
    item.ExtraWorn = item.ExtraWornLeft = false;
    return item;
}

// Reuse the real extra list; never synthesize an enchanted/tempered/quest item.
bool FindExtraList(Actor* apActor, TESObjectARMO* apArmor, const Inventory::Entry& acItem,
    ExtraDataList*& apResult) noexcept
{
    apResult = nullptr;
    const auto* changes = apActor->GetContainerChanges();
    if (!changes || !changes->entries) return false;
    for (auto* pEntry : *changes->entries)
    {
        if (!pEntry || pEntry->form != apArmor || !pEntry->dataList)
            continue;
        for (auto* pExtra : *pEntry->dataList)
        {
            if (!pExtra)
                continue;
            Inventory::Entry item;
            item.BaseId = acItem.BaseId;
            TESObjectREFR::GetItemFromExtraData(item, pExtra);
            if (SameInstance(item, acItem) && item.IsWorn() == acItem.IsWorn())
            {
                apResult = pExtra;
                return true;
            }
        }
    }
    // A nullptr extra list is valid only when positive plain stock remains
    // after subtracting all live instances, not merely when the wire item is plain.
    Inventory stock;
    if (Unworn(acItem).ContainsExtraData() || !ReadArmor(World::Get(), apActor, stock)) return false;
    int64_t plain = 0;
    for (const auto& item : stock.Entries)
        if (item.BaseId == acItem.BaseId && !item.ContainsExtraData()) plain += item.Count;
    return plain > 0;
}

thread_local bool s_replaying{};

std::string DescribeArmor(const Inventory& inventory)
{
    std::string text;
    for (const auto& item : inventory.Entries)
        if (item.Count > 0 && item.IsWorn())
            text += fmt::format(" {:X}:{:X}", item.BaseId.ModId, item.BaseId.BaseId);
    return text;
}
}

NakedNpcGuard::NakedNpcGuard(World& world, entt::dispatcher& dispatcher) noexcept
    : m_world(world)
    , m_assign(dispatcher.sink<AssignCharacterResponse>().connect<&NakedNpcGuard::OnAssign>(this))
    , m_spawn(dispatcher.sink<CharacterSpawnRequest>().connect<&NakedNpcGuard::OnSpawn>(this))
    , m_worn(dispatcher.sink<NotifyNpcWorn>().connect<&NakedNpcGuard::OnWorn>(this))
    , m_transfer(dispatcher.sink<NotifyOwnershipTransfer>().connect<&NakedNpcGuard::OnTransfer>(this))
    , m_disconnect(dispatcher.sink<DisconnectedEvent>().connect<&NakedNpcGuard::OnDisconnected>(this))
{
}

void NakedNpcGuard::Push(Input input) noexcept
{
    std::lock_guard lock(m_lock);
    m_input.push_back(std::move(input));
}

bool NakedNpcGuard::RemoteNpc(uint32_t id, uint32_t epoch) const noexcept
{
    if (!epoch) return false;
    for (auto entity : m_world.view<RemoteComponent>(entt::exclude<LocalComponent, PlayerComponent>))
    {
        const auto& remote = m_world.get<RemoteComponent>(entity);
        if (remote.Id == id && remote.OwnershipEpoch == epoch) return true;
    }
    return false;
}

bool NakedNpcGuard::Defer(const NotifyInventoryChanges& message) noexcept
{
    if (s_replaying || !RemoteNpc(message.ServerId, message.OwnershipEpoch)) return false;
    Push(message);
    return true;
}

bool NakedNpcGuard::Defer(const NotifyEquipmentChanges& message) noexcept
{
    if (s_replaying || !RemoteNpc(message.ServerId, message.OwnershipEpoch)) return false;
    Push(message);
    return true;
}

void NakedNpcGuard::WornSnapshot(uint32_t id, uint32_t epoch, const Inventory& worn) noexcept
{
    Push(Snapshot{id, epoch, 0, worn});
}

void NakedNpcGuard::OnAssign(const AssignCharacterResponse& message) noexcept
{
    if (!message.PlayerId && (message.InventoryAuthoritative || !message.CurrentInventory.Entries.empty()))
        Push(Snapshot{message.ServerId, message.OwnershipEpoch, 0, message.CurrentInventory, true});
}

void NakedNpcGuard::OnSpawn(const CharacterSpawnRequest& message) noexcept
{
    if (!message.IsPlayer) Push(Snapshot{message.ServerId, message.OwnershipEpoch, 0, message.InventoryContent, true});
}

void NakedNpcGuard::OnWorn(const NotifyNpcWorn& message) noexcept
{
    if (!message.Valid() || !message.Sequence) return;
    Snapshot snapshot{message.ServerId, message.OwnershipEpoch, message.Sequence, {}};
    for (const auto& item : message.Items) snapshot.Worn.Entries.push_back(item.Item);
    Push(std::move(snapshot)); // Intent never depends on root, process, or target scan readiness.
}

void NakedNpcGuard::OnTransfer(const NotifyOwnershipTransfer& message) noexcept
{
    std::lock_guard lock(m_lock);
    std::erase_if(m_targets, [&](const auto& target) { return target.Id == message.ServerId && target.Epoch < message.OwnershipEpoch; });
    m_input.push_back(Transfer{message.ServerId, message.OwnershipEpoch});
    m_nextTargets = 0;
}

void NakedNpcGuard::Reset() noexcept
{
    std::lock_guard lock(m_lock);
    m_input.clear(); m_targets.clear(); m_active = false;
    m_nextTargets = m_session = 0;
    ++m_generation;
}
void NakedNpcGuard::OnDisconnected(const DisconnectedEvent&) noexcept { Reset(); }

void NakedNpcGuard::Update() noexcept
{
    const bool connected = m_world.GetTransport().IsConnected();
    const auto session = m_world.GetPartyService().GetStartEpoch();
    std::lock_guard lock(m_lock);
    if (!connected || (m_session && session != m_session))
    {
        m_input.clear(); m_targets.clear(); ++m_generation;
        m_nextTargets = 0;
    }
    m_session = session; m_active = connected;
    const auto now = GetTickCount64();
    if (!connected || now < m_nextTargets) return;
    GuardCost cost;
    m_nextTargets = now + 250;
    m_targets.clear();
    for (auto entity : m_world.view<FormIdComponent, RemoteComponent>(entt::exclude<LocalComponent, PlayerComponent>))
    {
        const auto& remote = m_world.get<RemoteComponent>(entity);
        m_targets.push_back({m_world.get<FormIdComponent>(entity).Id, remote.Id, remote.OwnershipEpoch, false});
    }
}

void NakedNpcGuard::ApplyInput(const Input& input, uint64_t now) noexcept
{
    std::visit([&](const auto& message)
    {
        using T = std::decay_t<decltype(message)>;
        uint32_t id, epoch;
        if constexpr (std::is_same_v<T, Snapshot> || std::is_same_v<T, Transfer>)
        { id = message.Id; epoch = message.Epoch; }
        else { id = message.ServerId; epoch = message.OwnershipEpoch; }
        if (!epoch) return;
        auto& state = m_states[id];
        if (state.Epoch > epoch) return;
        if (state.Epoch != epoch) { state = {}; state.Epoch = epoch; }
        state.LastSeen = now;
        if constexpr (std::is_same_v<T, Transfer>) return;
        else if constexpr (std::is_same_v<T, Snapshot>)
        {
            // Only a full initial assignment/spawn is a contents baseline. A
            // worn snapshot (even complete/empty) says nothing about spare stock.
            if (message.Contents && !state.Complete && !state.Sequence && state.Changes.empty())
            {
                state.OwnerStock = message.Worn;
                state.OwnerStock.RemoveByFilter([&](const auto& item) { return !Armor(m_world, item.BaseId); });
                state.StockKnown = true;
            }
            // The legacy observer can re-seed an old snapshot after a new delta.
            // Sequenced notifications are authoritative; compatibility seeds only initialize.
            if ((!message.Sequence && (state.Complete || !state.Worn.Entries.empty())) ||
                (message.Sequence && message.Sequence <= state.Sequence)) return;
            state.Sequence = message.Sequence;
            state.Worn = message.Worn;
            state.Unmapped = std::any_of(state.Worn.Entries.begin(), state.Worn.Entries.end(), [&](const auto& item) {
                return item.Count > 0 && item.IsWorn() && !TESForm::GetById(m_world.GetModSystem().GetGameId(item.BaseId));
            });
            state.Worn.RemoveByFilter([&](const auto& item) { return item.Count <= 0 || !item.IsWorn() || !Armor(m_world, item.BaseId); });
            state.FormOnly.clear(); state.Complete = true;
        }
        else
        {
            if constexpr (std::is_same_v<T, NotifyInventoryChanges>)
            {
                int64_t ownerCount{};
                if (state.StockKnown && Armor(m_world, message.Item.BaseId))
                {
                    for (const auto& item : state.OwnerStock.Entries)
                        if (NpcSameItem(item, message.Item, false)) ownerCount += item.Count;
                    ownerCount = std::max<int64_t>(0, ownerCount + message.Item.Count);
                    state.OwnerStock.RemoveByFilter([&](const auto& item) { return NpcSameItem(item, message.Item, false); });
                    if (ownerCount > 0 && ownerCount <= INT32_MAX)
                    {
                        auto item = message.Item; item.Count = int32_t(ownerCount);
                        state.OwnerStock.Entries.push_back(std::move(item));
                    }
                    else if (ownerCount > INT32_MAX) state.StockKnown = false;
                }
                state.Changes.push_back(StockChange{message, ownerCount, state.StockKnown});
                if (!state.Sequence && message.Item.Count < 0)
                    state.Worn.RemoveByFilter([&](const auto& item) { return item.BaseId == message.Item.BaseId; });
            }
            else
            {
                state.Changes.push_back(message);
                if (!state.Sequence && !message.IsSpell && !message.IsShout)
                {
                    if (const auto* armor = Armor(m_world, message.ItemId))
                    {
                        state.Worn.RemoveByFilter([&](const auto& item) {
                            const auto* other = Armor(m_world, item.BaseId);
                            return item.BaseId == message.ItemId || (!message.Unequip && other && (other->slotType & armor->slotType));
                        });
                        std::erase(state.FormOnly, message.ItemId);
                        if (!message.Unequip)
                        {
                            Inventory::Entry item; item.BaseId = message.ItemId; item.Count = 1; item.ExtraWorn = true;
                            state.Worn.Entries.push_back(item); state.FormOnly.push_back(message.ItemId);
                        }
                    }
                }
            }
        }
        state.SettleUntil = now + kSettleMs;
        state.NextCheck = 0; state.Observe = true;
    }, input);
}

void NakedNpcGuard::UpdateNative() noexcept
{
    if (!NpcLootService::IsMainThread()) return;
    GuardCost cost;
    Vector<Input> input;
    Vector<Target> targets;
    uint64_t generation;
    bool active;
    {
        std::lock_guard lock(m_lock);
        generation = m_generation.load(); active = m_active;
        input.swap(m_input); targets = m_targets;
    }
    if (m_nativeGeneration != generation)
    {
        m_states.clear(); m_nativeGeneration = generation;
        m_nextHeartbeat = 0; m_checkedActors = 0; m_cursor = 0;
    }
    if (!active) return;
    const auto now = GetTickCount64();
    for (const auto& event : input) ApplyInput(event, now);
    if (!m_nextHeartbeat) m_nextHeartbeat = now + 30000;
    if (now >= m_nextHeartbeat)
    {
        spdlog::info("Naked guard: checked {} actors", m_checkedActors);
        m_checkedActors = 0; m_nextHeartbeat = now + 30000;
        std::erase_if(m_states, [&](const auto& entry) { return now - entry.second.LastSeen > 60000; });
    }

    // At most eight native actor checks per frame; inventory scans remain gated
    // by readiness, dirty input/lifecycle changes, and the per-actor deadline.
    for (size_t checked = 0; checked < std::min<size_t>(8, targets.size()); ++checked)
    {
        const auto& target = targets[m_cursor++ % targets.size()];
        auto it = m_states.find(target.Id);
        if (it == m_states.end() || it->second.Epoch != target.Epoch) continue;
        auto& state = it->second; state.LastSeen = now;
        if (generation != m_generation.load()) return;
        auto* actor = Cast<Actor>(TESForm::GetById(target.Form));
        if (!actor || !actor->GetExtension()->IsRemote() || actor->GetExtension()->IsPlayer()) continue;
        const void* root = actor->GetNiNode();
        const auto life = (actor->actorState.flags1 >> 21) & 15;
        const bool deathPending = ActorValueService::IsDeathPending(target.Form);
        if (root != state.Root) { state.Root = root; state.RootSince = now; state.NextCheck = 0; state.Observe = true; }
        if (life != state.Life)
        {
            // Died here: the worn list held now was published while the owner's actor was alive (e.g. Lokir's gag,
            // which the owner removes at death). Applying it re-equipped at the death transition and triggered the
            // native biped rebuild (LoadParts) while the death state had emptied the worn flags, rebuilding every slot
            // as skin: naked corpse (run 20260928-072831, 07:33:48). Wait up to 3 s for the owner's post-death list.
            if (life != 0 && (state.Life == 0 || state.Life == 0xff))
            {
                state.DeathSequence = state.Sequence;
                // 500 ms: long enough for the native death-time rebuild (LoadParts ~20 ms after the knock, run
                // 20260928-074345) to finish first, short enough that the corpse is not bare for seconds.
                state.DeathHoldUntil = now + 500;
            }
            state.Life = uint8_t(life);
            state.NextCheck = 0; state.Observe = true;
        }

        // Clothing is appearance, not a death-settle operation. Drain its stock
        // deltas in order on the native thread, then apply the latest complete
        // worn intent in this same visit. Other changes keep their existing hold.
        if (root && actor->currentProcess)
        {
            auto& inventory = m_world.ctx().at<InventoryService>();
            s_replaying = true;
            std::erase_if(state.Changes, [&](const auto& change) {
                return std::visit([&](const auto& message) {
                    using T = std::decay_t<decltype(message)>;
                    if constexpr (std::is_same_v<T, StockChange>)
                    {
                        if (!Armor(m_world, message.Message.Item.BaseId)) return false;
                        auto delivery = message.Message;
                        if (delivery.Item.Count > 0 && message.Known)
                        {
                            Inventory actual;
                            if (ReadArmor(m_world, actor, actual))
                            {
                                int64_t count{};
                                for (const auto& item : actual.Entries)
                                    if (NpcSameItem(item, delivery.Item, false)) count += item.Count;
                                delivery.Item.Count = WornDeliveryCount(delivery.Item.Count, message.OwnerCount, count);
                                if (delivery.Item.Count != message.Message.Item.Count)
                                    spdlog::info("Worn stock delivery: {:X} epoch {} item {:X}:{:X} delta {} applied {} owner-count {} local-count {}",
                                        target.Form, state.Epoch, delivery.Item.BaseId.ModId, delivery.Item.BaseId.BaseId,
                                        message.Message.Item.Count, delivery.Item.Count, message.OwnerCount, count);
                            }
                        }
                        if (delivery.Item.Count < 0)
                            state.FailedSupply.RemoveByFilter([&](const auto& item) { return item.BaseId == delivery.Item.BaseId; });
                        // AddOrRemoveItem otherwise implicitly equips by base form,
                        // losing instance identity before the worn-set application.
                        delivery.Item.ExtraWorn = delivery.Item.ExtraWornLeft = false;
                        if (delivery.Item.Count) inventory.OnNotifyInventoryChanges(delivery);
                        return true;
                    }
                    else
                    {
                        if (message.IsSpell || message.IsShout || !Armor(m_world, message.ItemId)) return false;
                        // Complete snapshots supersede form-only equipment deltas.
                        if (!state.Sequence) inventory.OnNotifyEquipmentChanges(message);
                        return true;
                    }
                }, change);
            });
            s_replaying = false;
        }
        if (!state.Changes.empty())
        {
            state.Held = state.Held || deathPending || life == 1 || life == 2;
            const bool wait = !root || deathPending || life == 1 ||
                (state.Held && (now < state.SettleUntil || now < state.RootSince + kSettleMs));
            if (wait)
            {
                if (!state.LoggedWait)
                    spdlog::info("Death-time hold waiting for server {}: {} changes, root {}, life {}, pending {}",
                        target.Id, state.Changes.size(), root, life, deathPending);
                state.LoggedWait = true;
            }
            else
            {
                Inventory before;
                if (state.Held && ReadArmor(m_world, actor, before))
                    spdlog::info("Death-time replay before server {}: owner [{}], worn [{}], root {}, life {}",
                        target.Id, DescribeArmor(state.Worn), DescribeArmor(before), root, life);
                uint32_t slots{};
                for (const auto& item : state.Worn.Entries)
                    if (const auto* armor = Armor(m_world, item.BaseId)) slots |= armor->slotType;
                if (state.Held) NpcLootService::TraceWornModels(actor, slots, "replay-before");
                auto& inventory = m_world.ctx().at<InventoryService>();
                s_replaying = true;
                for (const auto& change : state.Changes)
                    std::visit([&](const auto& message) {
                        using T = std::decay_t<decltype(message)>;
                        if constexpr (std::is_same_v<T, StockChange>) inventory.OnNotifyInventoryChanges(message.Message);
                        else inventory.OnNotifyEquipmentChanges(message);
                    }, change);
                s_replaying = false;
                if (state.Held)
                {
                    spdlog::info("Death-time hold released for server {}: {} changes applied", target.Id, state.Changes.size());
                    NpcLootService::TraceWornModels(actor, slots, "replay-after");
                }
                state.Changes.clear(); state.Held = state.LoggedWait = false;
                state.Observe = true; state.NextCheck = 0;
            }
        }
        // Native EquipObject has no dead/dying exclusion. Keep the root/process
        // readiness check, but never put the known worn set behind the content hold.
        if (!root || !actor->currentProcess || now < state.NextCheck) continue;
        if (life != 0 && state.Sequence == state.DeathSequence && now < state.DeathHoldUntil)
        {
            if (state.Observe)
                spdlog::info("Worn hold: {:X} died with pre-death worn seq {}; waiting for the owner's post-death list",
                    target.Form, state.Sequence);
            state.Observe = false;
            continue;
        }
        state.NextCheck = now + kCheckIntervalMs;
        ++m_checkedActors;
        Inventory stock;
        if (!ReadArmor(m_world, actor, stock))
        {
            if (state.Observe) spdlog::info("Naked guard deferred server {}: inventory unavailable", target.Id);
            state.Observe = false;
            continue;
        }
        const auto beforeWorn = DescribeArmor(stock);
        uint32_t expectedSlots{}, repaired{}, suppliedCount{}, unavailable{}, ambiguous{};
        for (const auto& desired : state.Worn.Entries)
        {
            auto* armor = Armor(m_world, desired.BaseId);
            if (!armor) continue;
            expectedSlots |= armor->slotType;
            const bool formOnly = std::find(state.FormOnly.begin(), state.FormOnly.end(), desired.BaseId) != state.FormOnly.end();
            Inventory::Entry candidate;
            int64_t total{}; bool found{}, multiple{};
            const auto select = [&] {
                total = 0; found = multiple = false;
                for (const auto& item : stock.Entries)
                {
                    if (item.BaseId != desired.BaseId) continue;
                    total += item.Count;
                    if (item.Count <= 0 || (!formOnly && !SameInstance(item, desired))) continue;
                    if (found && !SameInstance(candidate, item)) multiple = true;
                    // Preserve a matching worn instance, even if another plain
                    // stack precedes or follows it in the container changes.
                    if (!found || !candidate.IsWorn()) candidate = item;
                    found = true;
                }
            };
            select();
            if (multiple) { ++ambiguous; continue; }
            if (found && candidate.IsWorn() && total > 0) continue;
            if (total <= 0 || !found)
            {
                // A form-only delta cannot describe an enchanted/quest instance.
                // Until lazy loot excludes render-only copies, only reconstruct
                // stock independently present in the owner's contents baseline.
                // Do not mint an untracked lootable copy from appearance alone.
                int64_t ownerStock{};
                for (const auto& item : state.OwnerStock.Entries)
                    if (NpcSameItem(item, desired, false)) ownerStock += item.Count;
                if (!state.Complete || !state.StockKnown || ownerStock <= 0 || formOnly || desired.IsQuestItem || total < 0 ||
                    (total > 0 && std::any_of(state.FailedSupply.Entries.begin(), state.FailedSupply.Entries.end(),
                        [&](const auto& item) { return SameInstance(item, desired); })))
                {
                    if (state.Observe)
                        spdlog::info("Worn stock deferred: {:X} seq {} item {:X}:{:X} total {} candidate {} stock-known {} owner-stock {} form-only {} quest {}",
                            target.Form, state.Sequence, desired.BaseId.ModId, desired.BaseId.BaseId, total, found,
                            state.StockKnown, ownerStock, formOnly, desired.IsQuestItem);
                    ++unavailable;
                    continue;
                }
                auto addition = Unworn(desired);
                // Validate references before the existing serializer's native
                // enchant construction (which asserts on an unresolved enchant).
                bool mapped = !addition.ExtraEnchantId ||
                    (addition.ExtraEnchantId.ModId == UINT32_MAX ? !addition.EnchantData.Effects.empty() :
                        TESForm::GetById(m_world.GetModSystem().GetGameId(addition.ExtraEnchantId)) != nullptr);
                for (const auto& effect : addition.EnchantData.Effects)
                    mapped = mapped && TESForm::GetById(m_world.GetModSystem().GetGameId(effect.EffectId));
                if (!mapped) { ++unavailable; continue; }
                const auto beforeTotal = total;
                {
                    ScopedInventoryOverride inventoryOverride;
                    // Do not use AddOrRemoveItem's base-form auto-equip path.
                    // The engine owns this extra list; reacquire it after adding.
                    actor->AddObjectToContainer(Cast<TESBoundObject>(armor), TESObjectREFR::GetExtraDataFromItem(addition), 1, nullptr);
                }
                stock.Entries.clear();
                if (!ReadArmor(m_world, actor, stock)) { ++unavailable; continue; }
                select();
                if (total > beforeTotal)
                {
                    if (!found) state.FailedSupply.Entries.push_back(addition);
                    ++suppliedCount;
                }
                spdlog::info("Worn stock: {:X} server {} epoch {} seq {} item {:X}:{:X} total {} -> {} candidate {} life {} pending {}",
                    target.Form, target.Id, state.Epoch, state.Sequence, desired.BaseId.ModId, desired.BaseId.BaseId,
                    beforeTotal, total, found, life, deathPending);
                if (total <= 0 || !found) { ++unavailable; continue; }
            }
            ExtraDataList* extra{};
            if (!FindExtraList(actor, armor, candidate, extra)) { ++unavailable; continue; }
            ScopedInventoryOverride inventoryOverride;
            // 38894 is void (no success result), resolves a nullptr slot, and
            // the repository wrapper supplies ScopedEquipOverride. Native calls
            // occur only here on HookMainLoop, never under the input mutex.
            EquipManager::Get()->Equip(actor, armor, extra, 1, nullptr, false, false, false, true);
            Inventory after;
            if (ReadArmor(m_world, actor, after) && std::any_of(after.Entries.begin(), after.Entries.end(), [&](const auto& item) {
                return item.Count > 0 && item.IsWorn() && SameInstance(item, candidate);
            })) ++repaired;
            else ++unavailable;
            stock = std::move(after);
        }
        // Clear owner-empty armor slots only after replacement items are ready.
        // Never remove contents or detach an already-correct outfit to reapply it.
        if (state.Sequence && !state.Unmapped && !unavailable && !ambiguous)
            for (const auto& item : stock.Entries)
            {
                if (item.Count <= 0 || !item.IsWorn() || std::any_of(state.Worn.Entries.begin(), state.Worn.Entries.end(),
                    [&](const auto& desired) { return SameInstance(item, desired); })) continue;
                auto* armor = Armor(m_world, item.BaseId);
                ExtraDataList* extra{};
                if (armor && FindExtraList(actor, armor, item, extra))
                {
                    ScopedInventoryOverride inventoryOverride;
                    EquipManager::Get()->UnEquip(actor, armor, extra, 1, nullptr, false, true, false, true, nullptr);
                }
            }
        if (state.Observe || repaired || suppliedCount)
        {
            Inventory after; ReadArmor(m_world, actor, after);
            Vector<NpcWornItem> expected, actual;
            for (const auto& item : state.Worn.Entries)
                if (auto* armor = Armor(m_world, item.BaseId)) expected.push_back({armor->slotType, item});
            for (const auto& item : after.Entries)
                if (item.Count > 0 && item.IsWorn())
                    if (auto* armor = Armor(m_world, item.BaseId)) actual.push_back({armor->slotType, item});
            spdlog::info("Worn set: {:X} server {} epoch {} seq {} equipped {} supplied {} matches {} stock-known {} before [{}] after [{}] life {} pending {} held {}",
                target.Form, target.Id, state.Epoch, state.Sequence, repaired, suppliedCount,
                !state.Unmapped && NpcSameWorn(expected, actual), state.StockKnown, beforeWorn, DescribeArmor(after),
                life, deathPending, state.Changes.size());
            spdlog::info("Naked guard evidence server {} epoch {} seq {}: complete {}, owner [{}], worn [{}], unavailable {}, ambiguous {}, root {}, process {}, life {}",
                target.Id, target.Epoch, state.Sequence, state.Complete, DescribeArmor(state.Worn), DescribeArmor(after),
                unavailable, ambiguous, root, static_cast<const void*>(actor->currentProcess), life);
            NpcLootService::TraceWornModels(actor, expectedSlots, "guard-settled", state.Sequence);
            state.Observe = repaired || suppliedCount; // One later idempotence observation.
        }
    }
}
