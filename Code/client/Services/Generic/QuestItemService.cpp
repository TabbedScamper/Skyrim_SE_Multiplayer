#include <Services/Generic/QuestItemService.h>
#include <World.h>
#include <Components.h>
#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Forms/TESBoundObject.h>
#include <Events/EventDispatcher.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Services/PapyrusService.h>
#include <Games/Overrides.h>
#include <Interface/UI.h>
#include <FunctionHook.hpp>
#include <atomic>

namespace
{
std::atomic<QuestItemService*> s_service{};
std::atomic_uint64_t s_observeLeaderEpoch{};
uint64_t Token() noexcept
{
    static std::atomic_uint64_t next{(GetTickCount64() << 20) ^ GetCurrentProcessId()};
    return ++next;
}

bool Loading() noexcept
{
    auto* ui = UI::Get();
    return !ui || ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu"));
}

// Native IDs and ABI evidence are recorded in Code/campaign/QUEST_ITEM_RESEARCH.md.
// 12052 follows ExtraReferenceHandle before reading 0x88. Use the same indirection.
GamePtr<TESObjectREFR> InventoryReference(ExtraDataList* aList)
{
    if (!aList)
        return {};
    auto* extra = aList->GetByType(ExtraDataType::ReferenceHandle);
    if (!extra)
        return {};
    const auto handle = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(extra) + 0x10);
    return GamePtr<TESObjectREFR>(TESObjectREFR::GetByHandle(handle));
}

struct AliasVisitor
{
    virtual ~AliasVisitor() = default;
    virtual uint32_t Visit(BGSBaseAlias* aAlias)
    {
        if (aAlias && aAlias->owningQuest)
            Aliases.push_back(aAlias);
        return 1;
    }
    std::vector<BGSBaseAlias*> Aliases;
};

std::vector<BGSBaseAlias*> Aliases(ExtraDataList* aList)
{
    if (!aList)
        return {};
    auto reference = InventoryReference(aList);
    if (reference && (reference->flags & (1u << 5)))
        aList = reference->GetExtraDataList();
    auto* array = aList ? aList->GetByType(ExtraDataType::AliasInstanceArray) : nullptr;
    if (!array)
        return {};
    AliasVisitor visitor;
    using Visit = void(BSExtraData*, AliasVisitor*);
    POINTER_SKYRIMSE(Visit, visit, 12720);
    visit.Get()(array, &visitor);
    return std::move(visitor.Aliases);
}

ExtraDataList* ReferenceList(TESObjectREFR* aContainer, TESObjectREFR* aReference)
{
    auto* changes = aContainer ? aContainer->GetContainerChanges() : nullptr;
    if (!aReference || !changes || !changes->entries)
        return nullptr;
    for (auto* entry : *changes->entries)
    {
        if (!entry || entry->form != aReference->baseForm || !entry->dataList)
            continue;
        for (auto* list : *entry->dataList)
        {
            auto reference = InventoryReference(list);
            if (reference && reference->formID == aReference->formID)
                return list;
        }
    }
    return nullptr;
}

bool HasReference(TESObjectREFR* aContainer, TESObjectREFR* aReference)
{
    return ReferenceList(aContainer, aReference) != nullptr;
}

ExtraDataList* FindAliasList(TESObjectREFR* aContainer, TESForm* aBase, BGSBaseAlias* aAlias, bool aCopiesOnly)
{
    auto* changes = aContainer ? aContainer->GetContainerChanges() : nullptr;
    if (!changes || !changes->entries || !aAlias)
        return nullptr;
    for (auto* entry : *changes->entries)
        if (entry && entry->form == aBase && entry->dataList)
            for (auto* list : *entry->dataList)
            {
                if (aCopiesOnly && InventoryReference(list))
                    continue;
                for (auto* alias : Aliases(list))
                    if (alias == aAlias)
                        return list;
            }
    return nullptr;
}

int64_t Count(TESObjectREFR* aContainer, TESForm* aBase)
{
    if (!aContainer || !aBase || !aContainer->GetContainer())
        return 0;
    auto* changes = aContainer->GetContainerChanges();
    if (!changes || !changes->entries)
        return 0;
    return (std::max)(int64_t{0}, aContainer->GetItemCountInInventory(aBase));
}

void Notice(TESForm* aBase, uint32_t aCount, bool aAdded)
{
    using Show = void(TESForm*, uint32_t, bool, bool, const char*);
    POINTER_SKYRIMSE(Show, show, 51636);
    show.Get()(aBase, aCount, aAdded, true, nullptr);
}

// RemoveItem is latent: its native only queues work. Observe the functor after
// it actually removes inventory, not a base-count delta from death or theft.
using RemoveFunctor = void* (*)(void*, void*);
RemoveFunctor s_remove{};
void* HookRemove(void* aFunctor, void* aResult)
{
    auto* service = s_service.load();
    auto* bytes = static_cast<uint8_t*>(aFunctor);
    auto* owner = TESObjectREFR::GetByHandle(*reinterpret_cast<uint32_t*>(bytes + 0x10));
    const bool observe = service && service->Ready() && owner == PlayerCharacter::Get() &&
        World::Get().GetPartyService().IsLeader();
    if (observe)
        service->Discover();
    const auto before = observe ? service->CaptureCounts() : std::vector<std::pair<uint32_t, int64_t>>{};
    auto* result = s_remove(aFunctor, aResult);
    for (const auto& [baseId, count] : before)
        if (Count(owner, TESForm::GetById(baseId)) < count)
            service->ScriptRemoved(baseId);
    return result;
}

using ClearAlias = void (*)(TESQuest*, BGSBaseAlias*);
ClearAlias s_clear{};
void HookClear(TESQuest* aQuest, BGSBaseAlias* aAlias)
{
    auto* service = s_service.load();
    const auto epoch = s_observeLeaderEpoch.load();
    const bool observe = service && epoch && aQuest && aAlias;
    auto* oldReference = observe ? aQuest->GetAliasedRef(aAlias->aliasID) : nullptr;
    const uint32_t oldId = oldReference ? oldReference->formID : 0;
    const uint32_t questId = aQuest ? aQuest->formID : 0;
    const uint32_t instance = aQuest ? aQuest->currentInstanceID : 0;
    const uint32_t aliasId = aAlias ? aAlias->aliasID : 0;
    s_clear(aQuest, aAlias);
    if (observe && oldId)
        World::Get().GetRunner().Queue([questId, instance, aliasId, oldId, epoch]()
        {
            auto* current = s_service.load();
            if (current && World::Get().GetPartyService().GetStartEpoch() == epoch)
                current->AliasReleased(questId, instance, aliasId, oldId);
        });
}

using CompleteQuest = void (*)(TESQuest*, bool);
CompleteQuest s_complete{};
void HookComplete(TESQuest* aQuest, bool aComplete)
{
    s_complete(aQuest, aComplete);
    const auto epoch = s_observeLeaderEpoch.load();
    if (aComplete && aQuest && s_service.load() && epoch)
    {
        const auto questId = aQuest->formID;
        const auto instance = aQuest->currentInstanceID;
        World::Get().GetRunner().Queue([questId, instance, epoch]()
        {
            auto* current = s_service.load();
            if (current && World::Get().GetPartyService().GetStartEpoch() == epoch)
                current->QuestCompleted(questId, instance);
        });
    }
}

static TiltedPhoques::Initializer s_hooks([]()
{
    POINTER_SKYRIMSE(std::remove_pointer_t<RemoveFunctor>, remove, 56098);
    s_remove = remove.Get();
    TP_HOOK(&s_remove, HookRemove);
    POINTER_SKYRIMSE(std::remove_pointer_t<ClearAlias>, clear, 25051);
    s_clear = clear.Get();
    TP_HOOK(&s_clear, HookClear);
    POINTER_SKYRIMSE(std::remove_pointer_t<CompleteQuest>, complete, 24991);
    s_complete = complete.Get();
    TP_HOOK(&s_complete, HookComplete);
});
}

QuestItemService::QuestItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&QuestItemService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&QuestItemService::OnDisconnected>(this))
    , m_inventoryConnection(aDispatcher.sink<InventoryChangeEvent>().connect<&QuestItemService::OnInventory>(this))
    , m_notifyConnection(aDispatcher.sink<NotifyQuestItems>().connect<&QuestItemService::OnNotify>(this))
{
    s_service.store(this);
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

QuestItemService::~QuestItemService() noexcept
{
    s_observeLeaderEpoch.store(0);
    s_service.store(nullptr);
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
}

bool QuestItemService::Ready() const noexcept
{
    const auto& party = m_world.GetPartyService();
    auto* player = PlayerCharacter::Get();
    return m_ready && !m_applying && m_transport.IsConnected() && party.IsInParty() &&
        party.GetStartEpoch() == m_epoch && party.GetLeaderPlayerId() == m_leader &&
        party.GetSessionState() >= 2 && player && player->parentCell && player->GetNiNode() && !Loading();
}

void QuestItemService::Reset() noexcept
{
    s_observeLeaderEpoch.store(0);
    m_items.clear(); m_snapshot.clear(); m_retiring.clear(); m_pending.clear();
    m_referenceRetry.clear(); m_releasedReferences.clear();
    m_ready = false; m_epoch = 0; m_leader = 0; m_snapshotToken = 0;
    m_nextSnapshot = 0; m_nextScan = 0; m_page = 0;
}

void QuestItemService::OnDisconnected(const DisconnectedEvent&) noexcept { Reset(); }
BSTEventResult QuestItemService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    Reset();
    return BSTEventResult::kOk;
}

void QuestItemService::Send(QuestItemState aItem, QuestItemAction aAction) noexcept
{
    if (std::any_of(m_pending.begin(), m_pending.end(), [&](const auto& pending)
        { return pending.Request.Action == aAction && pending.Request.Item.SameKey(aItem); }))
        return;
    RequestQuestItems request;
    request.Epoch = m_epoch; request.Token = Token(); request.Action = aAction;
    aItem.Active = aAction == QuestItemAction::Acquire;
    request.Item = aItem;
    m_pending.push_back({request, GetTickCount64() + 2000});
    m_transport.Send(request);
}

bool QuestItemService::Retiring(const QuestItemState& aItem) const noexcept
{
    return std::any_of(m_retiring.begin(), m_retiring.end(), [&](const auto& item) { return item.SameKey(aItem); });
}

void QuestItemService::Discover() noexcept
{
    if (!Ready())
        return;
    auto* player = PlayerCharacter::Get();
    auto* changes = player->GetContainerChanges();
    if (!changes || !changes->entries)
        return;
    auto& mods = m_world.GetModSystem();
    for (auto* entry : *changes->entries)
    {
        if (!entry || !entry->form || !entry->dataList || Count(player, entry->form) <= 0)
            continue;
        for (auto* list : *entry->dataList)
        {
            for (auto* alias : Aliases(list))
            {
                auto* quest = alias->owningQuest;
                if (!alias->IsReference() || quest->IsStopped() || quest->flags == TESQuest::StopStart ||
                    (quest->flags & TESQuest::StageWait) || quest->unkFlags)
                    continue;
                QuestItemState item;
                if (!mods.GetServerModId(entry->form->formID, item.BaseId) ||
                    !mods.GetServerModId(quest->formID, item.QuestId))
                    continue;
                item.AliasId = alias->aliasID;
                item.QuestInstance = quest->currentInstanceID;
                item.QuestObject = (alias->flags & 4) != 0;
                auto* reference = quest->GetAliasedRef(alias->aliasID);
                if (reference && !reference->IsTemporary())
                    mods.GetServerModId(reference->formID, item.ReferenceId);
                // A reference alias identifies a single object. Never copy the whole
                // untagged base stack just because one instance is a quest object.
                item.Count = 1;
                if (item.IsValid() && !Retiring(item) &&
                    std::none_of(m_items.begin(), m_items.end(), [&](const auto& known) { return known.SameKey(item); }))
                    Send(item, QuestItemAction::Acquire);
            }
        }
    }
}

void QuestItemService::OnInventory(const InventoryChangeEvent& aEvent) noexcept
{
    if (aEvent.FormId == 0x14 && aEvent.Item.Count > 0 && !m_applying)
        Discover();
}

void QuestItemService::ScriptRemoved(uint32_t aBaseId) noexcept
{
    if (!Ready() || !m_world.GetPartyService().IsLeader())
        return;
    GameId base;
    if (!m_world.GetModSystem().GetServerModId(aBaseId, base))
        return;
    auto stillHeld = [&](const QuestItemState& item)
    {
        auto* quest = Cast<TESQuest>(TESForm::GetById(m_world.GetModSystem().GetGameId(item.QuestId)));
        auto* alias = quest ? quest->GetReferenceAlias(item.AliasId) : nullptr;
        auto* form = TESForm::GetById(aBaseId);
        return Count(PlayerCharacter::Get(), form) > 0 && FindAliasList(PlayerCharacter::Get(), form, alias, false);
    };
    for (const auto& item : m_items)
        if (item.Active && item.BaseId == base && !Retiring(item) && !stillHeld(item))
            m_retiring.push_back(item);
    for (const auto& pending : m_pending)
        if (pending.Request.Action == QuestItemAction::Acquire && pending.Request.Item.BaseId == base &&
            !Retiring(pending.Request.Item) && !stillHeld(pending.Request.Item))
            m_retiring.push_back(pending.Request.Item);
    for (const auto& item : m_items)
        if (item.Active && item.BaseId == base && Retiring(item))
            Send(item, QuestItemAction::Release);
}

std::vector<std::pair<uint32_t, int64_t>> QuestItemService::CaptureCounts() const
{
    std::map<uint32_t, int64_t> counts;
    auto capture = [&](const QuestItemState& item)
    {
        const auto id = m_world.GetModSystem().GetGameId(item.BaseId);
        if (item.Active && !counts.contains(id))
            counts[id] = Count(PlayerCharacter::Get(), TESForm::GetById(id));
    };
    for (const auto& item : m_items)
        capture(item);
    for (const auto& pending : m_pending)
        if (pending.Request.Action == QuestItemAction::Acquire)
            capture(pending.Request.Item);
    return {counts.begin(), counts.end()};
}

void QuestItemService::AliasReleased(uint32_t aQuestId, uint32_t aInstance, uint32_t aAliasId, uint32_t aOldReference) noexcept
{
    if (!Ready() || !m_world.GetPartyService().IsLeader())
        return;
    auto* quest = Cast<TESQuest>(TESForm::GetById(aQuestId));
    auto* current = quest ? quest->GetAliasedRef(aAliasId) : nullptr;
    // ForceRefTo can clear and immediately rebind the same alias. A transient
    // implementation detail is not a hand-in.
    if (current && quest->currentInstanceID == aInstance && current->formID == aOldReference)
        return;
    GameId id;
    if (!m_world.GetModSystem().GetServerModId(aQuestId, id))
        return;
    auto* oldReference = Cast<TESObjectREFR>(TESForm::GetById(aOldReference));
    if (oldReference && oldReference->baseForm && HasReference(PlayerCharacter::Get(), oldReference))
        m_releasedReferences[oldReference->baseForm->formID] = aOldReference;
    for (const auto& item : m_items)
        if (item.Active && item.QuestId == id && item.QuestInstance == aInstance && item.AliasId == aAliasId && !Retiring(item))
            m_retiring.push_back(item);
    for (const auto& pending : m_pending)
        if (pending.Request.Action == QuestItemAction::Acquire && pending.Request.Item.QuestId == id &&
            pending.Request.Item.QuestInstance == aInstance && pending.Request.Item.AliasId == aAliasId && !Retiring(pending.Request.Item))
            m_retiring.push_back(pending.Request.Item);
}

void QuestItemService::Merge(const QuestItemState& aItem) noexcept
{
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const auto& item) { return item.SameKey(aItem); });
    if (it != m_items.end() && it->Revision >= aItem.Revision)
        return;
    if (it == m_items.end())
        m_items.push_back(aItem);
    else
        *it = aItem;
    std::erase_if(m_pending, [&](const auto& p) { return p.Request.Item.SameKey(aItem) &&
        (p.Request.Item.Active == aItem.Active || aItem.Revision > p.Request.Item.Revision); });
    if (!aItem.Active)
        std::erase_if(m_retiring, [&](const auto& item) { return item.SameKey(aItem); });
}

void QuestItemService::QuestCompleted(uint32_t aQuestId, uint32_t aInstance) noexcept
{
    if (!Ready() || !m_world.GetPartyService().IsLeader())
        return;
    GameId id;
    if (!m_world.GetModSystem().GetServerModId(aQuestId, id))
        return;
    for (const auto& item : m_items)
        if (item.Active && item.QuestId == id && item.QuestInstance == aInstance && !Retiring(item))
            m_retiring.push_back(item);
    for (const auto& pending : m_pending)
        if (pending.Request.Action == QuestItemAction::Acquire && pending.Request.Item.QuestId == id &&
            pending.Request.Item.QuestInstance == aInstance && !Retiring(pending.Request.Item))
            m_retiring.push_back(pending.Request.Item);
}

void QuestItemService::OnNotify(const NotifyQuestItems& aMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!aMessage.IsValid() || !aMessage.ValidPayload() || !party.IsInParty() ||
        aMessage.Epoch != m_epoch || party.GetStartEpoch() != m_epoch || party.GetLeaderPlayerId() != m_leader)
        return;
    if (aMessage.Snapshot)
    {
        if (aMessage.Token != m_snapshotToken || aMessage.Page != m_page)
            return;
        ++m_page;
        m_snapshot.insert(m_snapshot.end(), aMessage.Items.begin(), aMessage.Items.end());
        if (!aMessage.Complete)
            return;
        for (const auto& item : m_snapshot)
            Merge(item);
        m_snapshot.clear(); m_snapshotToken = 0; m_ready = true;
        spdlog::info("Quest items: recovery snapshot ready, records={} epoch={}", m_items.size(), m_epoch);
    }
    else
        for (const auto& item : aMessage.Items)
            Merge(item);
    m_nextScan = 0;
}

void QuestItemService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty() || !party.GetStartEpoch())
    {
        if (m_epoch)
            Reset();
        return;
    }
    const bool loading = Loading();
    if (party.GetStartEpoch() != m_epoch || party.GetLeaderPlayerId() != m_leader || (loading && !m_loading))
        Reset();
    m_loading = loading;
    m_epoch = party.GetStartEpoch(); m_leader = party.GetLeaderPlayerId();
    auto* player = PlayerCharacter::Get();
    if (loading || !player || !player->parentCell || !player->GetNiNode() || party.GetSessionState() < 2)
    {
        s_observeLeaderEpoch.store(0);
        return;
    }
    const auto now = GetTickCount64();
    if (!m_ready && now >= m_nextSnapshot)
    {
        RequestQuestItems request;
        request.Epoch = m_epoch; request.Token = Token();
        m_snapshotToken = request.Token; m_page = 0; m_snapshot.clear();
        m_nextSnapshot = now + 5000;
        m_transport.Send(request);
    }
    s_observeLeaderEpoch.store(Ready() && party.IsLeader() ? m_epoch : 0);
    if (!Ready())
        return;
    for (auto& pending : m_pending)
        if (now >= pending.RetryAt)
        {
            pending.RetryAt = now + 2000;
            m_transport.Send(pending.Request);
        }
    if (now < m_nextScan)
        return;
    m_nextScan = now + 500;
    Discover();
    for (const auto& item : m_items)
        if (item.Active && Retiring(item))
            Send(item, QuestItemAction::Release);
    Reconcile();
}

void QuestItemService::Reconcile() noexcept
{
    auto* player = PlayerCharacter::Get();
    auto& mods = m_world.GetModSystem();
    const bool leader = m_world.GetPartyService().IsLeader();
    m_applying = true;
    // Keep the native events: player OnItemAdded and the real reference's
    // OnContainerChanged still run. This scope only suppresses inventory echo.
    ScopedInventoryOverride inventoryOverride;
    std::map<uint32_t, uint32_t> satisfied, required;
    for (const auto& item : m_items)
    {
        if (Retiring(item))
            continue;
        const auto baseId = mods.GetGameId(item.BaseId);
        auto* base = Cast<TESBoundObject>(TESForm::GetById(baseId));
        if (!base)
            continue;
        auto* quest = Cast<TESQuest>(TESForm::GetById(mods.GetGameId(item.QuestId)));
        auto* alias = quest ? quest->GetReferenceAlias(item.AliasId) : nullptr;
        if (!item.Active)
        {
            if (std::any_of(m_items.begin(), m_items.end(), [&](const auto& other) { return other.Active && other.BaseId == item.BaseId; }))
                continue;
            // Remove only instances still carrying a recorded alias. Ordinary
            // same-base items acquired after a hand-in are not party copies.
            auto* found = FindAliasList(player, base, alias, false);
            // Native Clear removes the alias metadata from the leader's real
            // reference before our durable retirement arrives.
            if (!found)
            {
                const auto released = m_releasedReferences.find(baseId);
                const auto referenceId = released != m_releasedReferences.end() ? released->second :
                    (item.ReferenceId ? mods.GetGameId(item.ReferenceId) : 0);
                found = ReferenceList(player, Cast<TESObjectREFR>(TESForm::GetById(referenceId)));
            }
            if (found && Count(player, base) > 0)
            {
                player->RemoveItem(base, item.Count, ITEM_REMOVE_REASON::kRemove, found, nullptr);
                player->UpdateItemList(nullptr);
                Notice(base, item.Count, false);
                spdlog::info("Quest items: removed party copy base={:X} revision={}", baseId, item.Revision);
            }
            continue;
        }
        if (!alias || !quest || quest->flags == TESQuest::StopStart || (quest->flags & TESQuest::StageWait) || quest->unkFlags)
            continue;
        auto* reference = quest->GetAliasedRef(item.AliasId);
        if (!reference && item.ReferenceId)
            reference = Cast<TESObjectREFR>(TESForm::GetById(mods.GetGameId(item.ReferenceId)));
        if (reference && reference->baseForm != base)
            continue;
        // Multiple aliases can reserve the same physical reference. Count it once.
        const uint32_t identity = reference ? reference->formID : baseId;
        if (satisfied.contains(identity))
            continue;
        satisfied[identity] = item.Count;
        required[baseId] += item.Count;

        if (leader && reference && !HasReference(player, reference))
        {
            const auto now = GetTickCount64();
            if (now < m_referenceRetry[identity])
                continue;
            PapyrusFunction<bool, TESObjectREFR, TESForm*, uint32_t, bool> add(
                m_world.ctx().at<PapyrusService>().Get("ObjectReference", "AddItem"));
            if (add)
            {
                // Replace an existing base-form entitlement with the real reference.
                // Native AddItem(reference) moves it and preserves alias/script identity.
                auto* copy = FindAliasList(player, base, alias, true);
                if (copy && Count(player, base) > 0)
                    player->RemoveItem(base, item.Count, ITEM_REMOVE_REASON::kRemove, copy, nullptr);
                add(player, reference, 1, false);
                m_referenceRetry[identity] = now + 3000;
                spdlog::info("Quest items: queued real reference {:X} for leader, base={:X}", reference->formID, baseId);
            }
            continue;
        }
        if (!leader && reference && HasReference(player, reference))
        {
            // Keep the aliased instance on the leader's local proxy too. Until
            // that actor is available, retain the item instead of deleting it.
            Actor* leaderActor = nullptr;
            auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
            for (auto entity : view)
                if (view.get<PlayerComponent>(entity).Id == m_leader)
                    leaderActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
            if (!leaderActor)
                continue;
            auto* list = FindAliasList(player, base, alias, false);
            if (list)
                player->RemoveItem(base, 1, ITEM_REMOVE_REASON::kStoreInContainer, list, leaderActor);
            if (HasReference(player, reference))
                continue;
        }
        auto count = Count(player, base);
        if (count >= required[baseId])
        {
            if ((leader && reference && HasReference(player, reference)) || FindAliasList(player, base, alias, true))
                continue;
            // An old inventory-sync copy or an old save may have the base form
            // without alias data. Replace just this entitlement to flag it.
            player->RemoveItem(base, item.Count, ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            count = Count(player, base);
        }
        // If a unique reference is unresolved after load, keep an interim
        // protected base copy. Replace it when the real reference resolves;
        // never ForceRefTo a fabricated object and discard the original scripts.
        auto* extra = ExtraDataList::New();
        using AddAlias = void(ExtraDataList*, TESQuest*, BGSBaseAlias*, void*);
        POINTER_SKYRIMSE(AddAlias, addAlias, 12046);
        // One object can fill several aliases, with the quest-object flag on
        // only one of them. Preserve every live reservation on its one copy.
        for (const auto& binding : m_items)
        {
            if (!binding.Active || Retiring(binding) || binding.BaseId != item.BaseId)
                continue;
            auto* boundQuest = Cast<TESQuest>(TESForm::GetById(mods.GetGameId(binding.QuestId)));
            auto* boundAlias = boundQuest ? boundQuest->GetReferenceAlias(binding.AliasId) : nullptr;
            auto* boundReference = boundQuest ? boundQuest->GetAliasedRef(binding.AliasId) : nullptr;
            const bool sameReference = reference && boundReference && reference->formID == boundReference->formID;
            if (boundAlias && (binding.SameKey(item) || sameReference ||
                (item.ReferenceId && binding.ReferenceId == item.ReferenceId)))
                addAlias.Get()(extra, boundQuest, boundAlias, nullptr);
        }
        const auto missing = static_cast<uint32_t>(required[baseId] - count);
        const bool questObject = extra->HasQuestObjectAlias();
        player->AddObjectToContainer(base, extra, missing, nullptr);
        player->UpdateItemList(nullptr);
        Notice(base, missing, true);
        spdlog::info("Quest items: restored base={:X} count={} nativeQuestObject={} revision={}",
            baseId, missing, questObject, item.Revision);
        if (leader && !reference)
            spdlog::warn("Quest items: base={:X} has an interim copy; unique alias reference is unresolved", baseId);
    }
    m_applying = false;
}

bool QuestItemService::HandleInventoryNotify(const NotifyInventoryChanges& aMessage) noexcept
{
    if (!m_world.GetPartyService().IsInParty() || (!aMessage.Item.IsQuestItem &&
        std::none_of(m_items.begin(), m_items.end(), [&](const auto& item) { return item.BaseId == aMessage.Item.BaseId; })))
        return false;
    // The generic path has an old opportunistic AddOrRemoveItem recursion that
    // gives a player an unflagged copy. Shared entitlements now own that operation.
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    for (auto entity : view)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        if (remote.Id != aMessage.ServerId || remote.OwnershipEpoch != aMessage.OwnershipEpoch)
            continue;
        if (auto* actor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id)))
        {
            ScopedInventoryOverride override;
            actor->AddOrRemoveItem(aMessage.Item, true);
        }
        return true;
    }
    return false;
}
