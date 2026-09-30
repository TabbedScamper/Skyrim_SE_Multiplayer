#include <Services/Generic/SharedDropService.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/NotifyPhysicsReferencesMove.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <ExtraData/ExtraCount.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESBoundObject.h>
#include <Interface/UI.h>
#include <FunctionHook.hpp>
#include <cmath>
#include <Games/ActorExtension.h>

// Death weapon drops (owner report 2026-09-29: the enemy's dropped weapon was not where the host saw it). The death
// path 0x1406A9E50 (37896) calls 0x140677C90 (37320) to drop the dying actor's weapons: always for some actors, else on
// a random roll against iDeathDropWeaponChance (setting 374997, value 374998). Every PC rolled its own dice for its own
// copy of the NPC, so a weapon fell on one PC and not the other, or in another spot. The NPC's owner alone drops: a
// copy's death skips it, and the owner's drop becomes a shared drop (one reference, seen by everyone, physics from the
// dropper) like a player's.
namespace
{
thread_local uint32_t t_deathDropActor{};
using TDeathDrop = void(Actor*);
TDeathDrop* s_realDeathDrop{};

void HookDeathDrop(Actor* apActor)
{
    auto* pExtension = apActor ? apActor->GetExtension() : nullptr;
    if (pExtension && pExtension->IsRemote() && !pExtension->IsPlayer() && World::Get().GetTransport().IsConnected())
    {
        static std::atomic<uint32_t> s_logs{};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 32)
            spdlog::info("Death drop of copy {:X} skipped: its owner's drop is shared", apActor->formID);
        return;
    }
    spdlog::info("Death drop of {:X}: dropping here (this PC owns it)", apActor ? apActor->formID : 0);
    const auto previous = t_deathDropActor;
    t_deathDropActor = apActor ? apActor->formID : 0;
    // As Actor::DropObject does for a player: the removal is the shared drop's to report (the server debits the
    // NPC when it creates the drop); a separate inventory change first left the drop unbacked.
    ScopedInventoryOverride inventoryOverride;
    s_realDeathDrop(apActor);
    t_deathDropActor = previous;
}

TiltedPhoques::Initializer s_deathDropHook([]()
{
    POINTER_SKYRIMSE(TDeathDrop, deathDrop, 37320);
    s_realDeathDrop = deathDrop.Get();
    TP_HOOK(&s_realDeathDrop, HookDeathDrop);
});
}

namespace
{
// 16065 / 0x1402310F0 emits a FORM ID at +0x10, not the handle that
// CommonLib's TESContainerChangedEvent declaration suggests. Read just these
// fields; the project's event declaration intentionally hides the remaining ABI.
struct ContainerChange
{
    uint32_t OldContainer, NewContainer, Base;
    int32_t Count;
    uint32_t Reference;
};
static_assert(sizeof(ContainerChange) == 0x14);

TESObjectREFR* Reference(uint32_t aId) { return Cast<TESObjectREFR>(TESForm::GetById(aId)); }
void SaveOwner(TESObjectREFR* aReference, bool aOwner)
{
    // BGSSaveLoadGame::AddChange (35576 / 0x140616CE0) refuses flag 0x4000.
    // Clear already registered changes before excluding a newly spawned proxy.
    // Do not call SetTemporary: it also removes the reference from allForms.
    constexpr uint32_t temporary = 1u << 14;
    if (aOwner)
    {
        aReference->flags &= ~temporary;
        aReference->MarkChanged(1 | TESObjectREFR::CHANGE_REFR_EXTRA_CREATED_ONLY |
            TESObjectREFR::CHANGE_REFR_MOVE | TESObjectREFR::CHANGE_REFR_HAVOK_MOVE);
    }
    else if (!(aReference->flags & temporary))
    {
        aReference->UnsetChanged(UINT32_MAX);
        aReference->flags |= temporary;
    }
}
void Notice(const String& aPlayer)
{
    if (aPlayer.empty()) return;
    const auto text = fmt::format("{} took it", aPlayer);
    using Show = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(Show, show, 52933);
    show.Get()(text.c_str(), nullptr, true);
}

bool ApplyExtra(TESObjectREFR* aReference, const SharedDropData& aData)
{
    auto& mods = World::Get().GetModSystem();
    auto* list = aReference->GetExtraDataList();
    const auto& item = aData.Item;
    EnchantmentItem* enchant = nullptr;
    if (item.ExtraEnchantId)
    {
        enchant = item.ExtraEnchantId.ModId == UINT32_MAX ? EnchantmentItem::Create(item.EnchantData) :
            Cast<EnchantmentItem>(TESForm::GetById(mods.GetGameId(item.ExtraEnchantId)));
        if (!enchant) return false;
        list->SetEnchantmentData(enchant, item.ExtraEnchantCharge, item.ExtraEnchantRemoveUnequip);
    }
    if (item.ExtraPoisonId)
    {
        auto* poison = Cast<AlchemyItem>(TESForm::GetById(mods.GetGameId(item.ExtraPoisonId)));
        if (!poison) return false;
        list->SetPoison(poison, item.ExtraPoisonCount);
    }
    if (aData.ExtraMask & 1) list->SetChargeData(item.ExtraCharge);
    if (aData.ExtraMask & 2) list->SetHealth(item.ExtraHealth);
    if (item.ExtraSoulLevel) list->SetSoulData(static_cast<SOUL_LEVEL>(item.ExtraSoulLevel));
    using SetCount = void(ExtraDataList*, uint16_t);
    POINTER_SKYRIMSE(SetCount, count, 11617);
    count.Get()(list, static_cast<uint16_t>(item.Count));
    if (!aData.Name.empty())
    {
        // CommonLib's constructor and SetName, with the engine-owned vtable so
        // Skyrim can serialize, copy and destroy this extra after native pickup.
        auto* text = Memory::Allocate<ExtraTextDisplayData>();
        if (!text) return false;
        memset(text, 0, sizeof(*text));
        POINTER_SKYRIMSE(void*, textVtable, 186855);
        *reinterpret_cast<void***>(text) = textVtable.Get();
        text->DisplayName.Set(aData.Name.c_str());
        text->iOwnerInstance = -2;
        text->fTemperFactor = 1.f;
        text->usCustomNameLength = static_cast<uint16_t>(aData.Name.size());
        using AddExtra = bool(ExtraDataList*, BSExtraData*);
        POINTER_SKYRIMSE(AddExtra, addExtra, 12315);
        addExtra.Get()(list, text);
    }
    return true;
}
}

SharedDropService::SharedDropService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SharedDropService::OnUpdate>(this))
    , m_notifyConnection(aDispatcher.sink<NotifySharedDrop>().connect<&SharedDropService::OnNotify>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&SharedDropService::OnDisconnected>(this))
{
    EventDispatcherManager::Get()->containerChangedEvent.RegisterSink(this);
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

SharedDropService::~SharedDropService() noexcept
{
    EventDispatcherManager::Get()->containerChangedEvent.UnRegisterSink(this);
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
}

bool SharedDropService::TracksPlayerDrops() const noexcept
{
    const auto& party = m_world.GetPartyService();
    return m_transport.IsConnected() && party.IsInParty() && party.GetStartEpoch() && party.GetSessionState() >= 2;
}

bool SharedDropService::Loading() const noexcept
{
    auto* ui = UI::Get();
    return !ui || ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu"));
}

BSTEventResult SharedDropService::OnEvent(const TESContainerChangedEvent* aEvent, const EventDispatcher<TESContainerChangedEvent>*)
{
    // Actor::DropObject holds this override while its native RemoveItem creates
    // the reference. Direct inventory removals already report their own delta.
    // Or the death drop of an NPC this PC owns (HookDeathDrop above), on this thread.
    const bool deathDrop = t_deathDropActor != 0;
    if (!aEvent || !TracksPlayerDrops() || (!ScopedInventoryOverride::IsOverriden() && !deathDrop)) return BSTEventResult::kOk;
    ContainerChange event{};
    memcpy(&event, aEvent, sizeof(event));
    const uint32_t dropper = deathDrop ? t_deathDropActor : 0x14;
    if (event.OldContainer != dropper || event.NewContainer || !event.Reference || event.Count <= 0) return BSTEventResult::kOk;
    if (deathDrop)
        spdlog::info("Death drop of {:X}: {:X} x{} shared", dropper, event.Base, event.Count);
    auto* reference = Reference(event.Reference);
    if (!reference || !reference->IsTemporary() || !reference->baseForm ||
        reference->baseForm->formID != event.Base || reference->GetExtraDataList()->HasQuestObjectAlias()) return BSTEventResult::kOk;
    std::lock_guard lock(m_lock);
    const auto token = m_nextToken++;
    m_nativeDrops.push_back({event.Reference, event.Base, event.Count, token, dropper});
    m_origins.emplace(token, event.Reference);
    return BSTEventResult::kOk;
}

BSTEventResult SharedDropService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    std::lock_guard lock(m_lock);
    m_loaded = true;
    return BSTEventResult::kOk;
}

void SharedDropService::Queue(const SharedDropData& aData, SharedDropAction aAction, uint64_t aToken) noexcept
{
    RequestSharedDrop message;
    static_cast<SharedDropData&>(message) = aData;
    message.Action = aAction; message.Token = aToken;
    m_outgoing.push_back(std::move(message));
}

bool SharedDropService::TryHold(TESObjectREFR* aReference, TESObjectREFR* aActivator) noexcept
{
    if (!aReference || !TracksPlayerDrops()) return false;
    std::lock_guard lock(m_lock);
    for (auto& [id, copy] : m_copies)
    {
        if (copy.FormId != aReference->formID) continue;
        // NPCs and script activators must not consume somebody else's linked copy.
        if (aActivator != PlayerCharacter::Get() || copy.Terminal || Loading()) return true;
        const auto now = GetTickCount64();
        if (!copy.Pending || now >= copy.Deadline)
        {
            copy.Pending = m_nextToken++; copy.Deadline = now + 300;
            Queue(copy.Data, SharedDropAction::Pickup, copy.Pending);
        }
        return true;
    }
    // Fence the original during the create round trip, even before it has a server ID.
    for (const auto& [token, form] : m_origins) if (form == aReference->formID) return true;
    return false;
}

TESObjectREFR* SharedDropService::Spawn(const SharedDropData& aData, bool aForPickup) noexcept
{
    auto* player = PlayerCharacter::Get();
    if (!player || !player->parentCell) return nullptr;
    auto& mods = m_world.GetModSystem();
    auto* base = Cast<TESBoundObject>(TESForm::GetById(mods.GetGameId(aData.Item.BaseId)));
    auto* cell = aForPickup ? player->parentCell : Cast<TESObjectCELL>(TESForm::GetById(mods.GetGameId(aData.Cell)));
    auto* space = aForPickup ? player->GetWorldSpace() : Cast<TESWorldSpace>(TESForm::GetById(mods.GetGameId(aData.WorldSpace)));
    if (!aForPickup && space)
        cell = ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(aData.Physics.Position.x / 4096.f)),
            static_cast<int32_t>(std::floor(aData.Physics.Position.y / 4096.f)), space, false);
    if (!base || !cell || !cell->IsAttached()) return nullptr;
    NiPoint3 position = aForPickup ? player->position : NiPoint3(aData.Physics.Position);
    NiPoint3 rotation(aData.Physics.Rotation);
    // 13723 / 0x1401BC1C0: CreateReferenceAtLocation, the same operation used by PlaceAtMe.
    using Create = uint32_t*(ModManager*, uint32_t*, TESForm*, NiPoint3*, NiPoint3*, TESObjectCELL*, TESWorldSpace*,
        TESObjectREFR*, uintptr_t, uintptr_t, char, char);
    POINTER_SKYRIMSE(Create, create, 13723);
    uint32_t handle{};
    create.Get()(ModManager::Get(), &handle, base, &position, &rotation, cell, space, nullptr, 0, 0, 0, 1);
    GamePtr<TESObjectREFR> reference(TESObjectREFR::GetByHandle(handle));
    if (!reference) return nullptr;
    if (!ApplyExtra(reference.operator->(), aData))
    {
        reference->Disable(); reference->Delete();
        return nullptr;
    }
    SaveOwner(reference.operator->(), aForPickup || aData.Owner == m_localPlayer);
    return reference.operator->();
}

void SharedDropService::RemoveCopy(Copy& aCopy, bool aKeep) noexcept
{
    auto* reference = Reference(aCopy.FormId);
    if (reference && reference->IsTemporary() && !reference->IsDeleted())
    {
        if (aKeep)
        {
            SaveOwner(reference, true);
        }
        else
        {
            // Restore change registration so an old saved authority reference is
            // retired in the owner's next save, too.
            SaveOwner(reference, true);
            reference->Disable(); reference->Delete();
            spdlog::info("Shared drop: {} copy removed", aCopy.Data.Id);
        }
    }
    aCopy.FormId = 0;
    aCopy.Ready = false;
}

void SharedDropService::Handle(const NotifySharedDrop& aMessage) noexcept
{
    if (!aMessage.IsValid() || !aMessage.ValidPayload() || aMessage.Epoch != m_epoch) return;
    if (aMessage.Action == SharedDropAction::Local)
    {
        m_origins.erase(aMessage.OriginToken);
        return;
    }
    auto it = m_copies.find(aMessage.Id);
    if (aMessage.Action == SharedDropAction::Upsert)
    {
        if (m_granted.count(aMessage.Id)) return;
        auto& copy = m_copies[aMessage.Id];
        if (copy.Terminal || copy.Data.Generation > aMessage.Generation) return;
        if (copy.FormId && copy.Data.Generation && copy.Data.Owner == m_localPlayer && aMessage.Owner != m_localPlayer)
            RemoveCopy(copy, false);
        if (copy.Data.Generation != aMessage.Generation) copy.Ready = false;
        copy.Data = aMessage;
        if (!copy.FormId && aMessage.Creator == m_transport.GetLocalPlayerId())
        {
            const auto origin = m_origins.find(aMessage.OriginToken);
            if (origin != m_origins.end()) { copy.FormId = origin->second; m_origins.erase(origin); }
        }
        return;
    }
    if (aMessage.Action == SharedDropAction::Granted)
    {
        if (m_granted.count(aMessage.Id)) return;
        auto& copy = m_copies[aMessage.Id];
        copy.Data = aMessage; copy.Grant = true; copy.Terminal = true; copy.Pending = 0;
        return;
    }
    if (it == m_copies.end()) return;
    auto& copy = it->second;
    if (aMessage.Action == SharedDropAction::Denied)
    {
        if (copy.Pending == aMessage.Token) { copy.Pending = 0; Notice(aMessage.Winner); }
    }
    else if (aMessage.Action == SharedDropAction::Remove)
    {
        if (copy.Pending) Notice(aMessage.Winner);
        copy.Pending = 0; copy.Terminal = true;
        if (!copy.Grant) RemoveCopy(copy, false);
    }
    else if (aMessage.Action == SharedDropAction::Release)
    {
        copy.Pending = 0; copy.Terminal = true;
        RemoveCopy(copy, aMessage.Owner == m_transport.GetLocalPlayerId());
    }
    else if (aMessage.Action == SharedDropAction::Move && !copy.Terminal &&
        copy.Data.Generation == aMessage.Generation && copy.Data.Owner != m_transport.GetLocalPlayerId())
    {
        copy.Data.Physics = aMessage.Physics;
        copy.Data.Tick = aMessage.Tick;
        m_physics.push_back(aMessage);
    }
}

void SharedDropService::OnMainFrame() noexcept
{
    std::lock_guard lock(m_lock);
    if (Loading()) return;
    const auto epoch = m_world.GetPartyService().GetStartEpoch();
    if (m_disconnected || m_loaded || (m_epoch && m_epoch != epoch))
    {
        const bool endedSession = !m_disconnected && !m_loaded && m_epoch && m_epoch != epoch;
        for (auto& [id, copy] : m_copies)
            RemoveCopy(copy, !m_loaded && copy.Data.Owner == m_localPlayer &&
                (endedSession || copy.Data.Replicas <= 1) && !copy.Terminal);
        m_copies.clear(); m_origins.clear(); m_nativeDrops.clear(); m_incoming.clear(); m_outgoing.clear(); m_physics.clear();
        m_granted.clear(); m_nextSnapshot = 0;
        m_disconnected = m_loaded = false;
    }
    m_epoch = epoch;
    if (!TracksPlayerDrops()) return;
    m_localPlayer = m_transport.GetLocalPlayerId();
    auto incoming = std::move(m_incoming); m_incoming.clear();
    for (const auto& message : incoming) Handle(message);
    for (const auto& native : m_nativeDrops)
    {
        auto* reference = Reference(native.Reference);
        if (!reference || reference->IsDeleted() || !reference->GetParentCellEx()) { m_origins.erase(native.Token); continue; }
        RequestSharedDrop request;
        request.Action = SharedDropAction::Create; request.Epoch = epoch; request.Token = native.Token;
        auto& mods = m_world.GetModSystem();
        auto* extras = reference->GetExtraDataList();
        mods.GetServerModId(native.Base, request.Item.BaseId);
        TESObjectREFR::GetItemFromExtraData(request.Item, extras);
        request.Item.Count = native.Count;
        request.Item.ExtraWorn = request.Item.ExtraWornLeft = false;
        if (native.Dropper != 0x14)
        {
            // A death drop: the server debits the NPC this PC owns.
            for (auto entity : m_world.view<FormIdComponent, LocalComponent>())
                if (m_world.get<FormIdComponent>(entity).Id == native.Dropper)
                {
                    request.Source = m_world.get<LocalComponent>(entity).Id;
                    break;
                }
            if (!request.Source) { m_origins.erase(native.Token); continue; }
        }
        request.ExtraMask = (extras->Contains(ExtraDataType::Charge) ? 1 : 0) | (extras->Contains(ExtraDataType::Health) ? 2 : 0);
        if (auto* text = Cast<ExtraTextDisplayData>(extras->GetByType(ExtraDataType::TextDisplayData)))
        {
            if (text->iOwnerInstance == -2 && text->DisplayName.AsAscii())
            {
                // DisplayName may already include a cached temper suffix. The
                // custom-name length excludes it; health recreates it on the copy.
                const auto length = strlen(text->DisplayName.AsAscii());
                request.Name.assign(text->DisplayName.AsAscii(), text->usCustomNameLength ?
                    (std::min)(length, static_cast<size_t>(text->usCustomNameLength)) : length);
            }
        }
        mods.GetServerModId(reference->GetParentCellEx()->formID, request.Cell);
        if (auto* space = reference->GetWorldSpace()) mods.GetServerModId(space->formID, request.WorldSpace);
        request.Physics.Position = {reference->position.x, reference->position.y, reference->position.z};
        request.Physics.Rotation = {reference->rotation.x, reference->rotation.y, reference->rotation.z};
        if (request.ValidPayload()) m_outgoing.push_back(std::move(request));
        else m_origins.erase(native.Token);
    }
    m_nativeDrops.clear();
    for (auto& [id, copy] : m_copies)
    {
        if (copy.Pending && GetTickCount64() >= copy.Deadline) copy.Pending = 0;
        if (copy.Grant)
        {
            // A grant is final even when its reliable packet arrives after the UI's
            // 300 ms hold. Never fall back to an unarbitrated native activation.
            auto* reference = Reference(copy.FormId);
            if (!reference || reference->IsDeleted()) reference = Spawn(copy.Data, true);
            if (!reference || !PlayerCharacter::Get()) continue;
            m_granted.insert(id); copy.Grant = false;
            SaveOwner(reference, true);
            ScopedInventoryOverride inventory;
            PlayerCharacter::Get()->PickUpObject(reference, copy.Data.Item.Count, false, 1.f);
            copy.FormId = 0;
            spdlog::info("Shared drop: {} picked up by local player", id);
            continue;
        }
        if (copy.Terminal) continue;
        auto* reference = Reference(copy.FormId);
        if (!reference || reference->IsDeleted())
        {
            reference = Spawn(copy.Data);
            if (!reference) continue;
            copy.FormId = reference->formID; copy.Ready = false;
        }
        if (!copy.Ready)
        {
            SaveOwner(reference, copy.Data.Owner == m_localPlayer);
            copy.Ready = true;
            Queue(copy.Data, SharedDropAction::Ready);
            if (copy.Data.Owner != m_transport.GetLocalPlayerId() && copy.Data.Physics.MotionType == 3)
            {
                NotifySharedDrop sample;
                static_cast<SharedDropData&>(sample) = copy.Data;
                sample.Action = SharedDropAction::Move;
                m_physics.push_back(std::move(sample));
            }
        }
    }
    if (GetTickCount64() >= m_nextSnapshot)
    {
        SharedDropData request; request.Epoch = epoch;
        Queue(request, SharedDropAction::Snapshot);
        m_nextSnapshot = GetTickCount64() + 2000;
    }
}

void SharedDropService::OnUpdate(const UpdateEvent&) noexcept
{
    std::vector<RequestSharedDrop> outgoing;
    { std::lock_guard lock(m_lock); outgoing.swap(m_outgoing); }
    for (const auto& message : outgoing) m_transport.Send(message);
}
void SharedDropService::OnNotify(const NotifySharedDrop& aMessage) noexcept
{
    std::lock_guard lock(m_lock); m_incoming.push_back(aMessage);
}
void SharedDropService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    std::lock_guard lock(m_lock); m_disconnected = true;
}
bool SharedDropService::IsShared(uint32_t aFormId) const noexcept
{
    std::lock_guard lock(m_lock);
    for (const auto& [id, copy] : m_copies) if (copy.FormId == aFormId && !copy.Terminal) return true;
    return false;
}
bool SharedDropService::IsOwner(uint32_t aFormId) const noexcept
{
    std::lock_guard lock(m_lock);
    for (const auto& [id, copy] : m_copies)
        if (copy.FormId == aFormId && !copy.Terminal) return copy.Data.Owner == m_transport.GetLocalPlayerId();
    return false;
}
GameId SharedDropService::PhysicsId(uint32_t aFormId) const noexcept
{
    std::lock_guard lock(m_lock);
    for (const auto& [id, copy] : m_copies) if (copy.FormId == aFormId && !copy.Terminal) return {SharedDropData::PhysicsModId, id};
    return {};
}
uint32_t SharedDropService::ResolvePhysics(const GameId& aId) const noexcept
{
    std::lock_guard lock(m_lock);
    const auto it = m_copies.find(aId.BaseId);
    return aId.ModId == SharedDropData::PhysicsModId && it != m_copies.end() && !it->second.Terminal &&
        it->second.Data.Owner != m_transport.GetLocalPlayerId() ? it->second.FormId : 0;
}
uint32_t SharedDropService::PhysicsGeneration(uint32_t aFormId) const noexcept
{
    std::lock_guard lock(m_lock);
    for (const auto& [id, copy] : m_copies)
        if (copy.FormId == aFormId && !copy.Terminal) return copy.Data.Generation;
    return 0;
}
std::vector<NotifySharedDrop> SharedDropService::TakePhysics() noexcept
{
    std::lock_guard lock(m_lock);
    std::vector<NotifySharedDrop> result;
    result.swap(m_physics);
    return result;
}
std::vector<uint32_t> SharedDropService::OwnedReferences() const noexcept
{
    std::lock_guard lock(m_lock);
    std::vector<uint32_t> result;
    for (const auto& [id, copy] : m_copies)
        if (copy.FormId && !copy.Terminal && copy.Data.Owner == m_localPlayer) result.push_back(copy.FormId);
    return result;
}
bool SharedDropService::HasRemoteReferences() const noexcept
{
    std::lock_guard lock(m_lock);
    for (const auto& [id, copy] : m_copies)
        if (copy.FormId && !copy.Terminal && copy.Data.Owner != m_localPlayer) return true;
    return false;
}
void SharedDropService::SendPhysics(uint32_t aFormId, const PhysicsReferenceUpdate& aPhysics, uint64_t aTick) noexcept
{
    std::lock_guard lock(m_lock);
    for (auto& [id, copy] : m_copies)
    {
        if (copy.FormId != aFormId || copy.Terminal || copy.Data.Owner != m_transport.GetLocalPlayerId()) continue;
        SharedDropData data = copy.Data; data.Physics = aPhysics; data.Tick = aTick;
        Queue(data, SharedDropAction::Move);
        break;
    }
}
