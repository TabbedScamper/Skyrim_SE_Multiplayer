#include <Services/Generic/UnstuckReset.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Services/CutsceneFollow.h>
#include <Services/CameraService.h>
#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <AI/Movement/PlayerControls.h>
#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSAction.h>
#include <Interface/UI.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Messages/NotifyPlayerControlState.h>
#include <BSAnimationGraphManager.h>
#include <FunctionHook.hpp>

namespace
{
// HUD cart mode is independent of camera state 12. Observe the native request
// from startup, including scripts outside multiplayer, rather than inferring it
// from the camera or reading a Scaleform stack while the UI thread mutates it.
std::atomic<bool> s_hudCartMode{};
using THudCart = void(void*, uint32_t, void*, bool);
THudCart* s_originalHudCart{};
void HookHudCart(void* apVM, uint32_t aStack, void* apTag, bool aEnabled)
{
    s_originalHudCart(apVM, aStack, apTag, aEnabled);
    s_hudCartMode.store(aEnabled, std::memory_order_release);
}
static TiltedPhoques::Initializer s_unstuckHooks([] {
    POINTER_SKYRIMSE(THudCart, cart, 55498);
    s_originalHudCart = cart.Get();
    TP_HOOK(&s_originalHudCart, HookHudCart);
});

// Research, 1.7.104 (kept here because docs/REFERENCE_RESEARCH.md is outside
// this task's edit list):
// https://github.com/alandtse/CommonLibSSE-NG/blob/ng/src/RE/A/Actor.cpp
// https://github.com/alandtse/CommonLibSSE-NG/blob/ng/src/RE/A/AIProcess.cpp
// https://github.com/ersh1/OpenAnimationReplacer/blob/main/src/Hooks.cpp
// https://github.com/ersh1/PairedAnimationImprovements/blob/main/src/Hooks.cpp
// Adopt native StopInteractingQuick (38697 / 1406D2A40) and StopCurrentIdle
// (39257 / 1406F0E30), including synchronized animation cancellation. OAR owns
// clip bindings across native activation/deactivation; PAI fixes synchronized
// annotations. Neither supplies a safe substitute for the native lifecycle.
// RevertAnimationGraphManager (32883 / 1405532C0) deactivates, initializes and
// reactivates graphs under native locks (140BC14E0, 1406B0340, 140BC0480).
// Vanilla load caller: 19531 / 1402E1180. Reject DoReset3D(true), 40255 /
// 140739A40: it removes biped parts. Never remove equipment or copy raw HKX
// pointers/graph variables from the host. A missing graph is deferred.
// Camera SetState (33026 / 140558F90) calls old End and new Begin. Required
// for cart (12): ForceThirdPerson (50796 / 1408F8F40) refuses states > 8.
// SetHudCartMode (55498 / 140A25650) uses HUD mode queue 51642 / 140936340.
// Furniture: native process handle/marker accessors 39916 / 140725300 and
// 39914 / 1407252C0; transition 39912 / 1407246A0. Reject normal player-use
// 40486 / 140749750, which calls UnequipObject. Use the process entry action
// instead (140722310, default action 55 from 11436 / 140154440).

uint32_t FurnitureHandle(Actor* apActor)
{
    uint32_t handle{};
    if (apActor && apActor->currentProcess)
    {
        using TGet = uint32_t*(AIProcess*, uint32_t*);
        POINTER_SKYRIMSE(TGet, get, 39916);
        get.Get()(apActor->currentProcess, &handle);
    }
    return handle;
}

uint32_t FurnitureMarker(Actor* apActor)
{
    using TGet = uint32_t(AIProcess*);
    POINTER_SKYRIMSE(TGet, get, 39914);
    return get.Get()(apActor->currentProcess);
}

bool Ready(World& aWorld)
{
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    if (!player || !player->parentCell || !player->currentProcess || !player->GetNiNode() ||
        !ui || ui->numPausesGame || ui->numItemMenus || ui->modal ||
        ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu")) ||
        ui->GetMenuOpen(BSFixedString("RaceSex Menu")) || aWorld.GetOverlayService().GetActive())
        return false;
    const auto life = (player->actorState.flags1 >> 21) & 0xF;
    return (life == 0 || life == 6) && player->animationGraphHolder.IsReady();
}

const char* RestoreFurniture(World& aWorld, PlayerCharacter* apPlayer, const PartyUnstuckState& acState)
{
    if (!acState.Furniture)
        return ((apPlayer->actorState.flags1 >> 14) & 0xF) == 0 ? "standing" : "exit-pending";
    auto* furniture = Cast<TESObjectREFR>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acState.Furniture)));
    if (!furniture || !furniture->GetNiNode() || furniture->IsDeleted() || furniture->IsDisabled() ||
        !furniture->baseForm || static_cast<uint8_t>(furniture->baseForm->formType) != 40 ||
        (furniture->parentCell != apPlayer->parentCell &&
            (!apPlayer->GetWorldSpace() || furniture->GetWorldSpace() != apPlayer->GetWorldSpace())))
        return "unavailable";
    // TESFurniture::associatedForm +E8 is a spell. Native sit completion casts
    // it (1407246A0 -> 1406D6C50); decline that side effect to preserve health.
    if (*reinterpret_cast<TESForm* const*>(reinterpret_cast<const uint8_t*>(furniture->baseForm) + 0xE8))
        return "associated-spell-skipped";
    // CommonLib BSFurnitureMarkerNode (+28 count), native lookup 76647 /
    // 140FF13D0, also used by TESFurniture::Activate (14026F340). A host marker
    // must exist in this client's loaded model before any native indexing.
    using TMarkers = const uint8_t*(NiNode*);
    POINTER_SKYRIMSE(TMarkers, markers, 76647);
    const auto* node = markers.Get()(furniture->GetNiNode());
    if (!node || acState.FurnitureMarker >= *reinterpret_cast<const uint32_t*>(node + 0x28))
        return "marker-unavailable";
    const auto handle = furniture->GetHandle().handle.iBits;
    if (FurnitureHandle(apPlayer) == handle && FurnitureMarker(apPlayer) == acState.FurnitureMarker &&
        ((apPlayer->actorState.flags1 >> 14) & 0xF) == 3)
        return "seated";
    using TAction = BGSAction*(uint32_t);
    POINTER_SKYRIMSE(TAction, action, 11436);
    auto* activate = action.Get()(55); // CommonLib DEFAULT_OBJECT::kActionActivate, not a form ID.
    if (!activate || !ActorMediator::Get())
        return "action-unavailable";
    using TSet = void(AIProcess*, Actor*, uint32_t, const uint32_t*, uint32_t);
    POINTER_SKYRIMSE(TSet, set, 39912);
    set.Get()(apPlayer->currentProcess, apPlayer, 1, &handle, acState.FurnitureMarker);
    TESActionData data(2, apPlayer, activate, furniture);
    if (ActorMediator::Get()->ForceAction(&data))
        return "entry-requested"; // Confirm actual seating in the paired run, not from dispatch.
    using TStop = void(Actor*, bool);
    POINTER_SKYRIMSE(TStop, stop, 38697);
    stop.Get()(apPlayer, true);
    return "entry-rejected";
}
}

UnstuckReset::UnstuckReset(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_beforeCameraConnection(aDispatcher.sink<UpdateEvent>().connect<&UnstuckReset::BeforeCamera>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&UnstuckReset::OnDisconnect>(this))
    , m_controlConnection(aDispatcher.sink<NotifyPlayerControlState>().connect<&UnstuckReset::OnControls>(this))
{
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

UnstuckReset::~UnstuckReset() noexcept
{
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
}

void UnstuckReset::ConnectUpdate(entt::dispatcher& aDispatcher) noexcept
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&UnstuckReset::OnUpdate>(this);
}

void UnstuckReset::BeforeCamera(const UpdateEvent&) noexcept
{
    m_reapplyControls = false;
    const auto& party = m_world.GetPartyService();
    if (!m_loaded.load(std::memory_order_acquire) && m_release && m_cleared &&
        m_world.GetTransport().IsConnected() && party.IsInParty() && !party.IsLeader() &&
        m_release->LeaderId == party.GetLeaderPlayerId() && m_release->Move.Epoch == party.GetStartEpoch() &&
        Ready(m_world) && CutsceneFollow::IsActive())
    {
        // The normal party callback may rearm the current scene. Release its
        // mirror BEFORE CameraService can replay the host's scripted idle again.
        CutsceneFollow::Update(m_world, false, false, m_release->LeaderId);
        m_reapplyControls = true;
    }
}

BSTEventResult UnstuckReset::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    m_loaded.store(true, std::memory_order_release);
    return BSTEventResult::kOk;
}

bool UnstuckReset::Capture(RequestPartyUnstuck& aRequest) noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    auto* controls = PlayerControls::GetInstance();
    auto& state = aRequest.State;
    if (!Ready(m_world) || !camera || !camera->state || !controls || !UnstuckControls::Capture(state.Controls))
        return false;
    state.Controls.Epoch = aRequest.Move.Epoch;
    state.Controls.Sequence = aRequest.Move.Sequence;
    state.Sneaking = (player->actorState.flags1 & 0x200) != 0;
    state.WeaponDrawn = player->actorState.IsWeaponDrawn();
    state.FirstPerson = camera->state->id == 0;
    state.CartMode = camera->state->id == 12;
    state.HudCartMode = s_hudCartMode.load(std::memory_order_acquire);
    // SetAIDriven 40586 / 1407559F0 owns this bit and native movement-controller callbacks.
    state.AIDriven = (*(reinterpret_cast<const uint8_t*>(player) + 0xBEA) & 8) != 0;
    state.InputBlocked = controls->bBlockPlayerInput;
    state.Furniture = {};
    state.FurnitureMarker = 0;
    if (((player->actorState.flags1 >> 14) & 0xF) == 3)
    {
        auto* furniture = TESObjectREFR::GetByHandle(FurnitureHandle(player));
        if (furniture && m_world.GetModSystem().GetServerModId(furniture->formID, state.Furniture))
            state.FurnitureMarker = FurnitureMarker(player);
        else
            state.Furniture = {};
    }
    return state.IsValid(aRequest.Move);
}

void UnstuckReset::Queue(const NotifyPartyUnstuck& acMessage) noexcept
{
    // TriggerGate has already checked transport, leader, epoch and replay order.
    // Transport packets and UpdateEvent are dispatched on World::Update's thread.
    m_pending = acMessage;
    m_loaded.store(false, std::memory_order_release);
    m_release = acMessage.State.CartMode ? std::nullopt : std::make_optional(acMessage);
    m_sawFree = acMessage.State.Controls.Free;
    m_controlSequence = 0;
    m_expires = GetTickCount64() + 10000;
    m_arrivalAfter = 0;
    m_cleared = false;
    m_graphReset = false;
}

void UnstuckReset::OnDisconnect(const DisconnectedEvent&) noexcept
{
    m_pending.reset();
    m_release.reset();
}

void UnstuckReset::OnControls(const NotifyPlayerControlState& acMessage) noexcept
{
    if (!m_release || acMessage.LeaderId != m_release->LeaderId ||
        acMessage.State.Epoch != m_release->Move.Epoch || !acMessage.State.IsValid() ||
        acMessage.State.Sequence <= m_controlSequence)
        return;
    m_controlSequence = acMessage.State.Sequence;
    // Cancel this scene's mirror until the host has been free and begins a NEW
    // scene. Do not let the slower LeaderControl heartbeat undo F8 next frame.
    if (m_sawFree && !acMessage.State.Free)
        m_release.reset();
    else
    {
        m_sawFree |= acMessage.State.Free;
        m_release->State.Controls = acMessage.State;
    }
}

bool UnstuckReset::Apply(const NotifyPartyUnstuck& acMessage, bool aRestoreFurniture) noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    if (!camera || !camera->state)
        return false;
    const auto& state = acMessage.State;
    bool graph = true;
    if (!aRestoreFurniture)
    {
        if (!state.CartMode)
            CutsceneFollow::Update(m_world, false, false, acMessage.LeaderId);
        using TStop = void(Actor*, bool);
        POINTER_SKYRIMSE(TStop, stop, 38697);
        stop.Get()(player, true);
        using TIdle = void(AIProcess*, Actor*, bool);
        POINTER_SKYRIMSE(TIdle, idle, 39257);
        idle.Get()(player->currentProcess, player, true);
        // End the graph-driven camera before resetting its graph, as CameraService does.
        if (!state.CartMode)
        {
            BSFixedString end("IdleWalkingCameraEnd");
            player->animationGraphHolder.SendAnimationEvent(&end);
            CameraService::NoteWalkingCameraIdle(0, false);
        }
        graph = player->animationGraphHolder.RevertAnimationGraphManager();
        m_graphReset = graph;
        if (!graph)
        {
            // Revert can stop before its final activation if a holder callback
            // fails. Always try the native activation pass before returning, so
            // that a failed reset does not deliberately leave a stopped graph.
            BSAnimationGraphManager* manager{};
            if (player->animationGraphHolder.GetBSAnimationGraph(&manager) && manager)
            {
                using TActivate = bool(BSAnimationGraphManager*);
                POINTER_SKYRIMSE(TActivate, activate, 63356); // 140BC0480
                activate.Get()(manager);
            }
            if (manager)
                manager->Release();
        }
    }
    // SetAIDriven can exit a native camera; apply the final camera and controls afterwards.
    using TAI = void(PlayerCharacter*, bool);
    POINTER_SKYRIMSE(TAI, ai, 40586);
    const bool aiDriven = (*(reinterpret_cast<const uint8_t*>(player) + 0xBEA) & 8) != 0;
    if (aiDriven != state.AIDriven)
        ai.Get()(player, state.AIDriven);
    // Only the native sneak bit and its corresponding graph variable, as in StealthService.
    // Full actor-state writes would overwrite life, attack and movement ownership.
    player->actorState.flags1 = (player->actorState.flags1 & ~0x200u) | (state.Sneaking ? 0x200u : 0u);
    BSFixedString sneak("isSneaking");
    player->animationGraphHolder.SetVariableBool(&sneak, state.Sneaking);
    // The graph has just reverted even when the actor's drawn bit already
    // matches. The existing Ex wrapper forces native draw/sheath evaluation.
    player->SetWeaponDrawnEx(state.WeaponDrawn);
    const char* furniture = aRestoreFurniture ? RestoreFurniture(m_world, player, state) : "cleared";
    if (!state.HudCartMode)
    {
        // Same queued native HUD path as Game.SetHudCartMode(false).
        using THud = void(const char*, bool);
        POINTER_SKYRIMSE(THud, hud, 51642);
        hud.Get()("CartMode", false);
        s_hudCartMode.store(false, std::memory_order_release);
    }
    if (!state.CartMode)
    {
        // Game.SetCameraTarget uses 50838 / 1408FB250. Merely changing the
        // camera state leaves a scripted target behind; restore the player first.
        using TTarget = void(PlayerCamera*, Actor*);
        POINTER_SKYRIMSE(TTarget, target, 50838);
        target.Get()(camera, player);
        const uint8_t desired = state.FirstPerson ? 0 : 9;
        if (camera->state->id != desired)
        {
            // ForceFirstPerson (50790 / 1408F8DE0) accepts cart once its target
            // is the player. Stage through it so ForceThirdPerson initializes
            // its own state before native SetState calls End/Begin.
            camera->ForceFirstPerson();
            if (!state.FirstPerson)
                camera->ForceThirdPerson();
        }
    }
    uint32_t before{}, after{};
    const bool applied = UnstuckControls::Apply(state, before, after);
    spdlog::info("Unstuck reset: controls {:X}->{:X} furniture {} graph {} camera {}",
        before, after, furniture, m_graphReset ? (aRestoreFurniture ? "retained" : "reset") : "failed", camera->state->id);
    if (!aRestoreFurniture && applied)
    {
        using TShow = void(const char*, const char*, bool);
        POINTER_SKYRIMSE(TShow, show, 52933);
        show.Get()("Party state reset by the host.", nullptr, true);
    }
    return applied;
}

void UnstuckReset::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_loaded.exchange(false, std::memory_order_acq_rel))
    {
        m_pending.reset();
        m_release.reset();
        return;
    }
    // EnTT publishes in connection order. BeforeCamera releases the scene, then
    // CameraService restores its cached controls, then this callback applies the
    // host policy last. No cell-follow policy is modified.
    const auto& party = m_world.GetPartyService();
    if (m_release)
    {
        if (!m_world.GetTransport().IsConnected() || !party.IsInParty() || party.IsLeader() ||
            m_release->LeaderId != party.GetLeaderPlayerId() || m_release->Move.Epoch != party.GetStartEpoch())
            m_release.reset();
        else if (m_reapplyControls && Ready(m_world))
        {
            uint32_t before{}, after{};
            UnstuckControls::Apply(m_release->State, before, after);
        }
    }
    if (!m_pending)
        return;
    const auto now = GetTickCount64();
    const auto& message = *m_pending;
    if (!m_world.GetTransport().IsConnected() || !party.IsInParty() || party.IsLeader() ||
        message.LeaderId != party.GetLeaderPlayerId() || message.Move.Epoch != party.GetStartEpoch() || now > m_expires)
    {
        if (now > m_expires)
            spdlog::warn("Unstuck reset: expired sequence={} cleared={} before furniture arrival", message.Move.Sequence, m_cleared);
        m_pending.reset();
        return;
    }
    if (!Ready(m_world))
        return;
    if (!m_cleared)
    {
        m_cleared = Apply(message, false);
        m_arrivalAfter = now + 100;
        return;
    }
    if (now < m_arrivalAfter)
        return;
    auto* player = PlayerCharacter::Get();
    auto& mods = m_world.GetModSystem();
    const auto cell = mods.GetGameId(message.Move.CellId);
    auto* world = player->GetWorldSpace();
    const bool arrived = message.Move.WorldSpaceId ? world && world->formID == mods.GetGameId(message.Move.WorldSpaceId) :
        !world && player->parentCell->formID == cell;
    // Do not initiate furniture use from the old cell. The existing TriggerGate
    // MoveTo remains the sole relocation decision; no new teleport or gather.
    const float dx = player->position.x - message.Move.Position.x;
    const float dy = player->position.y - message.Move.Position.y;
    const float dz = player->position.z - message.Move.Position.z;
    const float radius = 128.f + 48.f * static_cast<float>(message.Slot / 3);
    if (arrived && dx * dx + dy * dy + dz * dz <= radius * radius && Apply(message, true))
        m_pending.reset();
}
