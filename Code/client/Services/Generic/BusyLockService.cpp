#include <TiltedOnlinePCH.h>
#include <Services/Generic/BusyLockService.h>
#include <Services/Generic/DialogueListenService.h>
#include <World.h>
#include <Components.h>
#include <Utils.h>
#include <PlayerCharacter.h>
#include <Forms/TESBoundObject.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Interface/UI.h>
#include <Events/ActivateEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/BusyLockRequest.h>
#include <Messages/NotifyBusyLock.h>
#include <FunctionHook.hpp>
#include <cmath>

// Research for the coordinator's docs/REFERENCE_RESEARCH.md entry (that document
// is outside this task's edit allowlist): CommonLibSSE-NG source, inspected locally
// in C:/Tools/ref/CommonLibSSE-NG and at:
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/include/RE/U/UI.h
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/C/ContainerMenu.cpp
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/B/BarterMenu.cpp
// Adopt its UI+8 event source and target handles; a global menu-only lock was
// rejected because independent references must remain parallel.
// Offline 1.7.104 sk.py source inspected before adding any hooks:
// 19796 / 0x1402F11B0 ActivateRef: scripts and virtual base-form activation.
// 19781 / 0x1402F08D0 GetDisplayFullName: honors renamed references for HUD text.
// 40548 / 0x140751580 ActivatePickRef: player input return site +0x112.
// 24715 / 0x1403C05C0 NPC activation: corpse mode 0, pickpocket mode 2, dialogue.
// 17889 / 0x140282130 container activation -> ContainerMenu::OpenMenu.
// 51140 / 0x140911FB0 ContainerMenu::OpenMenu: target handle then queued show.
// 50955 / 0x140901CC0 BarterMenu::OpenMenu: merchant handle then queued show.
// 82082 / 0x141169D20 UI message processing: sends open/close events at UI+8.
// 82485 / 0x141177A90 UI strings: Dialogue Menu, ContainerMenu, BarterMenu.
// 52933 / 0x140991A30 ShowHUDMessage: use the existing CreatorTogether ABI.
// Reuse the existing ActivateRef hook, not a second detour. Observe menus by
// target, and retain the lease across dialogue/barter transitions. Direct native
// inventory/barter opens also pass through the same lease state machine.

struct MenuOpenCloseEvent
{
    BSFixedString menuName;
    bool opening;
    uint8_t pad09[7];
};
static_assert(sizeof(MenuOpenCloseEvent) == 0x10);
static_assert(offsetof(MenuOpenCloseEvent, opening) == 8);

namespace
{
std::atomic<BusyLockService*> s_service{};
using TOpenContainer = void(TESObjectREFR*, int32_t);
using TOpenBarter = void(Actor*);
TOpenContainer* s_openContainer{};
TOpenBarter* s_openBarter{};

void HookOpenContainer(TESObjectREFR* aReference, int32_t aMode)
{
    auto* service = s_service.load();
    if (!service || !service->TryHoldMenu(aReference, false, aMode))
        s_openContainer(aReference, aMode);
}

void HookOpenBarter(Actor* aMerchant)
{
    auto* service = s_service.load();
    if (!service || !service->TryHoldMenu(aMerchant, true, 0))
        s_openBarter(aMerchant);
}

uint32_t MenuBit(const char* aName)
{
    if (!aName) return 0;
    if (std::strcmp(aName, "Dialogue Menu") == 0) return 1;
    if (std::strcmp(aName, "ContainerMenu") == 0) return 2;
    if (std::strcmp(aName, "BarterMenu") == 0) return 4;
    if (std::strcmp(aName, "Lockpicking Menu") == 0) return 8;
    return 0;
}

void Notice(const std::string& aText)
{
    using TShow = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(TShow, show, 52933);
    show.Get()(aText.c_str(), nullptr, true);
}

TiltedPhoques::Initializer s_hooks([]()
{
    POINTER_SKYRIMSE(TOpenContainer, container, 51140);
    POINTER_SKYRIMSE(TOpenBarter, barter, 50955);
    s_openContainer = container.Get();
    s_openBarter = barter.Get();
    TP_HOOK(&s_openContainer, HookOpenContainer);
    TP_HOOK(&s_openBarter, HookOpenBarter);
});
}

BusyLockService::BusyLockService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&BusyLockService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&BusyLockService::OnDisconnected>(this))
    , m_notifyConnection(aDispatcher.sink<NotifyBusyLock>().connect<&BusyLockService::OnNotify>(this))
{
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
    EventDispatcherManager::Get()->deathEvent.RegisterSink(this);
    s_service.store(this);
}

BusyLockService::~BusyLockService() noexcept
{
    s_service.store(nullptr);
    if (m_menuSource)
        m_menuSource->UnRegisterSink(this);
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
    EventDispatcherManager::Get()->deathEvent.UnRegisterSink(this);
}

bool BusyLockService::Ready() const noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    return m_transport.IsOnline() && m_world.GetPartyService().IsInParty() &&
        m_world.GetPartyService().GetPartyMembers().size() >= 2 && player &&
        !player->IsDead() && !player->actorState.IsBleedingOut() && ui &&
        !ui->GetMenuOpen(BSFixedString("Loading Menu")) && !ui->GetMenuOpen(BSFixedString("Main Menu"));
}

bool BusyLockService::GetReferenceId(TESObjectREFR* aReference, GameId& aId) noexcept
{
    // Never lease another player (including a downed party member).
    if (auto* actor = Cast<Actor>(aReference); actor && actor->GetExtension()->IsPlayer())
        return false;
    if (aReference->formID >> 24 != 0xFF)
        return m_world.GetModSystem().GetServerModId(aReference->formID, aId) && aId && aId.ModId != UINT32_MAX;
    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != aReference->formID)
            continue;
        if (auto id = Utils::GetServerId(entity))
        {
            aId = GameId(UINT32_MAX, *id);
            return true;
        }
    }
    return false;
}

bool BusyLockService::TryHold(TESObjectREFR* aReference, TESObjectREFR* aActivator, uint8_t aUnk1,
    TESBoundObject* aObject, int32_t aCount, char aDefaultProcessing, const void* aCaller) noexcept
{
    using TPick = void();
    POINTER_SKYRIMSE(TPick, pick, 40548);
    // Scripted Activate calls can have exactly the same arguments as player input.
    if (reinterpret_cast<uintptr_t>(aCaller) != reinterpret_cast<uintptr_t>(pick.Get()) + 0x112 ||
        aActivator != PlayerCharacter::Get() || !aReference || !aReference->baseForm)
        return false;
    auto* actor = Cast<Actor>(aReference);
    // A locked door or container opens the lockpicking minigame: one player at a time (two could pick at once).
    const auto* lock = !actor ? aReference->GetLock() : nullptr;
    const bool locked = lock && lock->IsLocked();
    if (!actor && aReference->baseForm->formType != FormType::Container && !locked)
        return false;
    Activation activation;
    activation.Object = aObject ? aObject->formID : 0;
    activation.Count = aCount;
    activation.Unk1 = aUnk1;
    activation.DefaultProcessing = aDefaultProcessing;
    if (locked)
        return Begin(aReference, activation, BusyLockKind::Lockpicking);
    const bool searching = !actor || actor->IsDead() || (PlayerCharacter::Get()->actorState.flags1 & (1u << 9));
    return Begin(aReference, activation, searching ? BusyLockKind::Searching : BusyLockKind::Speaking);
}

bool BusyLockService::TryHoldMenu(TESObjectREFR* aReference, bool aBarter, int32_t aMode) noexcept
{
    if (!aReference || !m_acceptMenuRequests.load())
        return false;
    const auto handle = aReference->GetHandle().handle.iBits;
    const auto lifecycle = m_invalidations.load();
    // These natives can be called by scripts. Keep registry/transport state and
    // the delayed native replay on the world runner, using a generational handle.
    m_world.GetRunner().Queue([this, handle, lifecycle, aBarter, aMode]()
    {
        if (s_service.load() != this || lifecycle != m_invalidations.load())
            return;
        auto* reference = TESObjectREFR::GetByHandle(handle);
        if (!reference || reference->IsDeleted() || reference->IsDisabled())
            return;
        Activation activation;
        activation.Type = aBarter ? Operation::Barter : Operation::Container;
        activation.Mode = aMode;
        if (Begin(reference, activation, aBarter ? BusyLockKind::Bartering : BusyLockKind::Searching))
            return;
        if (aBarter)
        {
            if (auto* actor = Cast<Actor>(reference))
                s_openBarter(actor);
        }
        else
            s_openContainer(reference, aMode);
    });
    return true;
}

bool BusyLockService::Begin(TESObjectREFR* aReference, const Activation& aActivation, BusyLockKind aKind) noexcept
{
    if (!aReference || !Ready() || aReference->IsDeleted() || aReference->IsDisabled())
        return false;
    GameId reference{};
    if (!GetReferenceId(aReference, reference))
        return false;
    if (m_request.RequestId && m_request.Reference == reference &&
        m_request.Epoch == m_world.GetPartyService().GetStartEpoch() && m_lifecycle == m_invalidations.load())
    {
        // Native menu opening during replay or dialogue -> barter uses the same lease.
        if (!m_waiting && m_request.Kind != aKind)
        {
            m_request.Kind = aKind;
            m_nextHeartbeat = 0;
        }
        m_closedAt = 0;
        return m_waiting;
    }
    Reset(BusyLockReason::Cancelled);
    auto* cell = PlayerCharacter::Get()->GetParentCellEx();
    if (!cell)
        return false;
    m_activation = aActivation;
    m_activation.Handle = aReference->GetHandle().handle.iBits;
    m_activation.Cell = cell->formID;
    m_activation.WorldSpace = cell->worldspace ? cell->worldspace->formID : 0;
    m_request.Reference = reference;
    m_request.Epoch = m_world.GetPartyService().GetStartEpoch();
    m_request.RequestId = ++m_nextRequest;
    m_request.Kind = aKind;
    m_lifecycle = m_invalidations.load();
    m_waiting = true;
    m_deadline = GetTickCount64() + 300;
    m_menuEvents.store(0);
    BusyLockRequest request;
    static_cast<BusyLockData&>(request) = m_request;
    if (!m_transport.Send(request))
    {
        Reset(BusyLockReason::Cancelled);
        return false;
    }
    return true;
}

void BusyLockService::Send(const BusyLockData& aData, BusyLockAction aAction, BusyLockReason aReason) noexcept
{
    if (!aData.RequestId || !m_transport.IsOnline())
        return;
    BusyLockRequest request;
    static_cast<BusyLockData&>(request) = aData;
    request.Action = aAction;
    request.Reason = aReason;
    request.Holder.clear();
    m_transport.Send(request);
}

void BusyLockService::Reset(BusyLockReason aReason) noexcept
{
    Send(m_request, BusyLockAction::Release, aReason);
    m_request = {};
    m_activation = {};
    m_waiting = m_leased = m_seenMenu = false;
    m_closedAt = m_deadline = m_openDeadline = m_nextHeartbeat = 0;
}

bool BusyLockService::CanReplay(TESObjectREFR* aReference) noexcept
{
    if (!aReference || !Ready() || m_lifecycle != m_invalidations.load() ||
        m_request.Epoch != m_world.GetPartyService().GetStartEpoch() ||
        aReference->IsDeleted() || aReference->IsDisabled())
        return false;
    auto* cell = aReference->GetParentCellEx();
    auto* player = PlayerCharacter::Get();
    auto* playerCell = player->GetParentCellEx();
    if (!playerCell || (playerCell->worldspace ? playerCell->worldspace->formID : 0) != m_activation.WorldSpace ||
        (!m_activation.WorldSpace && playerCell->formID != m_activation.Cell))
        return false;
    if (m_activation.Type != Operation::Activate)
        return true;
    if (!cell || (cell->worldspace ? playerCell->worldspace != cell->worldspace : playerCell != cell))
        return false;
    const auto delta = aReference->position - player->position;
    const float distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    return std::isfinite(distance) && distance <= 400.f * 400.f;
}

void BusyLockService::Replay() noexcept
{
    auto* reference = TESObjectREFR::GetByHandle(m_activation.Handle);
    if (!CanReplay(reference))
    {
        Reset(BusyLockReason::Cancelled);
        return;
    }
    m_waiting = false;
    m_openDeadline = GetTickCount64() + 15000;
    m_nextHeartbeat = 0;
    const auto activation = m_activation;
    if (activation.Type == Operation::Container)
        s_openContainer(reference, activation.Mode);
    else if (activation.Type == Operation::Barter)
    {
        if (auto* actor = Cast<Actor>(reference))
            s_openBarter(actor);
        else
            Reset(BusyLockReason::Cancelled);
    }
    else
    {
        auto* object = activation.Object ? Cast<TESBoundObject>(TESForm::GetById(activation.Object)) : nullptr;
        // Activate() calls the original trampoline. Preserve the existing activation
        // replication exactly once, only after the interaction has been permitted.
        m_world.GetRunner().Trigger(ActivateEvent(reference, PlayerCharacter::Get(), object,
            activation.Count, activation.DefaultProcessing, activation.Unk1, TESObjectREFR::kNone));
        // A false return can still have queued script processing; wait for menu events.
        reference->Activate(PlayerCharacter::Get(), activation.Unk1, object, activation.Count, activation.DefaultProcessing);
    }
}

void BusyLockService::OnNotify(const NotifyBusyLock& aMessage) noexcept
{
    if (!aMessage.IsValid())
        return;
    const bool matches = aMessage.RequestId == m_request.RequestId && aMessage.Epoch == m_request.Epoch &&
        aMessage.Reference == m_request.Reference;
    if (!matches || !m_waiting)
    {
        // Duplicate current grants are harmless. Late grants after timeout/close are
        // explicitly retired without touching a newer request for the same reference.
        if (aMessage.Action == BusyLockAction::Granted && !(matches && m_leased))
            Send(aMessage, BusyLockAction::Release, BusyLockReason::Cancelled);
        return;
    }
    if (!Ready() || m_lifecycle != m_invalidations.load() ||
        m_request.Epoch != m_world.GetPartyService().GetStartEpoch())
    {
        Reset(BusyLockReason::Cancelled);
        return;
    }
    if (GetTickCount64() >= m_deadline)
    {
        Send(m_request, BusyLockAction::Release, BusyLockReason::Timeout);
        spdlog::info("Busy lock: {:X} request timed out; allowing activation", m_request.Reference.LogFormat());
        Replay();
        return;
    }
    if (aMessage.Action == BusyLockAction::Denied)
    {
        auto* reference = TESObjectREFR::GetByHandle(m_activation.Handle);
        using TName = const char*(TESObjectREFR*);
        POINTER_SKYRIMSE(TName, displayName, 19781);
        const auto* name = reference ? displayName.Get()(reference) : nullptr;
        if (aMessage.Kind == BusyLockKind::Speaking && reference && aMessage.HolderPlayerId)
        {
            // Busy talking with another player: listen in on that conversation instead.
            Notice(fmt::format("{} is speaking with {}. Listening in.", name && *name ? name : "This person", aMessage.Holder));
            DialogueListen::Begin(reference->formID, aMessage.HolderPlayerId);
        }
        else if (aMessage.Kind == BusyLockKind::Speaking || aMessage.Kind == BusyLockKind::Bartering)
            Notice(fmt::format("{} is speaking with {}", name && *name ? name : "This person", aMessage.Holder));
        else if (aMessage.Kind == BusyLockKind::Lockpicking)
            Notice(fmt::format("{} is picking this lock", aMessage.Holder));
        else
            Notice(fmt::format("{} is searching this", aMessage.Holder));
        Reset(BusyLockReason::Cancelled);
        return;
    }
    if (aMessage.Action != BusyLockAction::Granted)
    {
        // An explicit stale/cancelled request response is not a network timeout.
        Reset(BusyLockReason::Cancelled);
        return;
    }
    m_leased = true;
    Replay();
}

uint32_t BusyLockService::OpenMenus(TESObjectREFR* aReference) const noexcept
{
    auto* ui = UI::Get();
    if (!ui || !aReference)
        return 0;
    uint32_t mask{};
    if (ui->GetMenuOpen(BSFixedString("Dialogue Menu")) && MenuTopicManager::IsPlayerDialogueSpeaker(aReference))
        mask |= 1;
    POINTER_SKYRIMSE(uint32_t, containerHandle, 405962);
    POINTER_SKYRIMSE(uint32_t, barterHandle, 405823);
    if (ui->GetMenuOpen(BSFixedString("ContainerMenu")) && TESObjectREFR::GetByHandle(*containerHandle.Get()) == aReference)
        mask |= 2;
    if (ui->GetMenuOpen(BSFixedString("BarterMenu")) && TESObjectREFR::GetByHandle(*barterHandle.Get()) == aReference)
        mask |= 4;
    // The minigame only opens for the reference this lease activated.
    if (ui->GetMenuOpen(BSFixedString("Lockpicking Menu")) && aReference->GetHandle().handle.iBits == m_activation.Handle)
        mask |= 8;
    return mask;
}

void BusyLockService::OnUpdate(const UpdateEvent&) noexcept
{
    m_acceptMenuRequests.store(Ready());
    if (!m_menuSource)
    {
        if (auto* ui = UI::Get())
        {
            // Verified in ID 82082, not an inferred UI padding offset.
            m_menuSource = reinterpret_cast<EventDispatcher<MenuOpenCloseEvent>*>(reinterpret_cast<uint8_t*>(ui) + 8);
            m_menuSource->RegisterSink(this);
        }
    }
    const auto events = m_menuEvents.exchange(0);
    if (!m_request.RequestId)
        return;
    if (m_lifecycle != m_invalidations.load())
    {
        Reset(m_invalidationReason.load());
        return;
    }
    if (!Ready() || m_request.Epoch != m_world.GetPartyService().GetStartEpoch())
    {
        auto* player = PlayerCharacter::Get();
        Reset(player && (player->IsDead() || player->actorState.IsBleedingOut()) ? BusyLockReason::Death : BusyLockReason::PartyLeft);
        return;
    }
    const auto now = GetTickCount64();
    if (m_waiting)
    {
        if (now >= m_deadline)
        {
            Send(m_request, BusyLockAction::Release, BusyLockReason::Timeout);
            spdlog::info("Busy lock: {:X} request timed out; allowing activation", m_request.Reference.LogFormat());
            Replay();
        }
        return;
    }
    auto* reference = TESObjectREFR::GetByHandle(m_activation.Handle);
    auto* cell = PlayerCharacter::Get()->GetParentCellEx();
    if (!reference || reference->IsDeleted() || reference->IsDisabled() || !cell ||
        (cell->worldspace ? cell->worldspace->formID : 0) != m_activation.WorldSpace ||
        (!m_activation.WorldSpace && cell->formID != m_activation.Cell))
    {
        Reset(BusyLockReason::Load);
        return;
    }
    const auto menus = OpenMenus(reference);
    // An open and close can both occur between two world updates.
    if ((events & 15) & (events >> 8))
        m_seenMenu = true;
    if (menus)
    {
        m_seenMenu = true;
        m_closedAt = 0;
        const auto kind = (menus & 8) ? BusyLockKind::Lockpicking : (menus & 4) ? BusyLockKind::Bartering :
            (menus & 2) ? BusyLockKind::Searching : BusyLockKind::Speaking;
        if (kind != m_request.Kind)
            m_nextHeartbeat = 0;
        m_request.Kind = kind;
    }
    else if (m_seenMenu)
    {
        // A close event is a signal to recheck all target menus, not an immediate
        // release: dialogue and barter can hand over during the same UI queue drain.
        if (!m_closedAt || (events & 15))
            m_closedAt = now;
        if (now - m_closedAt >= 250)
        {
            Reset(BusyLockReason::Closed);
            return;
        }
    }
    else if (now >= m_openDeadline)
    {
        Reset(BusyLockReason::Cancelled);
        return;
    }
    if (m_leased && now >= m_nextHeartbeat)
    {
        Send(m_request, BusyLockAction::Heartbeat);
        m_nextHeartbeat = now + 15000;
    }
}

void BusyLockService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_acceptMenuRequests.store(false);
    ++m_invalidations;
    Reset(BusyLockReason::Disconnected);
}

BSTEventResult BusyLockService::OnEvent(const MenuOpenCloseEvent* aEvent, const EventDispatcher<MenuOpenCloseEvent>*)
{
    if (aEvent)
    {
        m_menuEvents.fetch_or(MenuBit(aEvent->menuName.AsAscii()) << (aEvent->opening ? 0 : 8));
        const auto* name = aEvent->menuName.AsAscii();
        if (aEvent->opening && name && (std::strcmp(name, "Loading Menu") == 0 || std::strcmp(name, "Main Menu") == 0))
        {
            m_invalidationReason.store(BusyLockReason::Load);
            m_acceptMenuRequests.store(false);
            ++m_invalidations;
        }
    }
    return BSTEventResult::kOk;
}

BSTEventResult BusyLockService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_invalidationReason.store(BusyLockReason::Load);
    m_acceptMenuRequests.store(false);
    ++m_invalidations;
    return BSTEventResult::kOk;
}

BSTEventResult BusyLockService::OnEvent(const TESDeathEvent* aEvent, const EventDispatcher<TESDeathEvent>*)
{
    if (aEvent && aEvent->isDead && aEvent->pActorDying == PlayerCharacter::Get())
    {
        m_invalidationReason.store(BusyLockReason::Death);
        m_acceptMenuRequests.store(false);
        ++m_invalidations;
    }
    return BSTEventResult::kOk;
}
