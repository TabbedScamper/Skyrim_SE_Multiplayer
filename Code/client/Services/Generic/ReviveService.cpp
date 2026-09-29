#include <TiltedOnlinePCH.h>
#include <Services/ReviveService.h>
#include <Services/PlayerCollision.h>
#include <World.h>
#include <Components.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <AI/AIProcess.h>
#include <EquipManager.h>
#include <Interface/UI.h>
#include <Interface/ControlBindings.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/ReviveRequest.h>
#include <Messages/NotifyRevive.h>
#include <Games/Animation/TESActionData.h>
#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <Games/TES.h>
#include <mutex>
#include <Games/Animation/ActorMediator.h>
#include <Forms/BGSAction.h>
#include <xinput.h>
#include <cmath>

namespace
{
void Notice(const std::string& aText) noexcept
{
    using TShow = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(TShow, show, 52933);
    show.Get()(aText.c_str(), nullptr, true);
}

void LifeState(Actor* aActor, uint32_t aState) noexcept
{
    using TSet = void(Actor*, uint32_t);
    POINTER_SKYRIMSE(TSet, set, 37612);
    set.Get()(aActor, aState);
}

// A remote copy's bleedout, played like its owner's replayed actions. Our PerformAction hook (38949) blocks every
// engine action on remote copies, so SetLifeState's own ActionBleedoutStart/Stop never runs on them, and a raw graph
// event did not take the copy's graph out of bleedout: it stayed lying while the owner's pose stream kept it standing,
// and the host saw the revived player snap between lying and standing (2026-09-28). Skyrim.esm: ActionBleedoutStart
// 13EC9 -> idle BleedoutStart 13ECC "bleedOutStart"; ActionBleedoutStop 13ECA -> idle BleedoutStop 13ECE "bleedOutStop".
bool PlayRemoteBleedout(Actor* aActor, bool aStart) noexcept
{
    auto* action = Cast<BGSAction>(TESForm::GetById(aStart ? 0x13EC9 : 0x13ECA));
    auto* idle = Cast<TESIdleForm>(TESForm::GetById(aStart ? 0x13ECC : 0x13ECE));
    if (!action || !idle)
        return false;
    TESActionData data(0, aActor, action, nullptr);
    data.eventName = BSFixedString(aStart ? "bleedOutStart" : "bleedOutStop");
    data.idleForm = idle;
    return ActorMediator::Get()->ForceAction(&data);
}

void EndBleedout(Actor* aActor) noexcept
{
    LifeState(aActor, 0);
    BSFixedString stop("BleedoutStop");
    aActor->SendAnimationEvent(&stop);
    aActor->SetNoBleedoutRecovery(true);
}

void Recover(Actor* aActor, float aFraction) noexcept
{
    // Current health includes damage; subtracting that modifier includes temporary max-health buffs.
    const float maximum = aActor->GetActorValue(ActorValueInfo::kHealth) - aActor->healthModifiers.damageModifier;
    aActor->SetNoBleedoutRecovery(false);
    const float before = aActor->GetActorValue(ActorValueInfo::kHealth);
    const float target = (std::max)(1.f, maximum * aFraction);
    aActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, target);
    EndBleedout(aActor);
    spdlog::info("Revive: health {:.1f} -> {:.1f} (target {:.1f} of max {:.1f}, permanent {:.1f})", before,
        aActor->GetActorValue(ActorValueInfo::kHealth), target, maximum,
        aActor->GetActorPermanentValue(ActorValueInfo::kHealth));
}

struct Mapping
{
    const char* Event;
    uint16_t Key;
    uint16_t Modifier;
    uint8_t Rest[12];
};
static_assert(sizeof(Mapping) == 0x18);

struct ActivateInput
{
    bool Held{};
    std::string Label{"Activate"};
    // Keyboard/mouse key name, and the Activate button on the controller ("a", "b", "x", "y", "lb", "rb", "lt", "rt").
    std::string Key;
    std::string Button;
    // A controller was used more recently than the keyboard/mouse: prompts show its glyph.
    bool Gamepad{};
};

// Last controller activity (XInput packet number change) vs the last keyboard/mouse input (GetLastInputInfo).
DWORD s_lastPacket{};
uint64_t s_lastPadInput{};

const char* PadButtonName(uint16_t aKey) noexcept
{
    switch (aKey)
    {
    case XINPUT_GAMEPAD_A: return "a";
    case XINPUT_GAMEPAD_B: return "b";
    case XINPUT_GAMEPAD_X: return "x";
    case XINPUT_GAMEPAD_Y: return "y";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "lb";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "rb";
    case 9: return "lt";
    case 10: return "rt";
    default: return "a";
    }
}

// The live bindings of a control-map user event ("Activate", "Shout"): keyboard key, mouse button, controller button.
ActivateInput ReadControl(const char* aEvent) noexcept
{
    ActivateInput result;
    result.Label = aEvent;
    POINTER_SKYRIMSE(uint8_t*, map, 400863);
    auto* controls = *map.Get();
    auto* context = controls ? *reinterpret_cast<uint8_t**>(controls + 0x60) : nullptr;
    if (!context)
        return result;
    using TXInput = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static TXInput getState = []() -> TXInput {
        for (auto* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (const auto module = LoadLibraryW(library))
                if (const auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<TXInput>(proc);
        return nullptr;
    }();
    for (uint32_t device = 0; device < 3; ++device)
    {
        const auto& mappings = *reinterpret_cast<const GameArray<Mapping>*>(context + device * 0x18);
        for (const auto& entry : mappings)
        {
            if (!entry.Event || std::strcmp(entry.Event, aEvent) != 0 || entry.Key == 0xFF)
                continue;
            if (device == 0)
            {
                const auto scan = entry.Key & 0x80 ? 0xE000 | (entry.Key & 0x7F) : entry.Key;
                const auto key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
                char label[64]{};
                GetKeyNameTextA((entry.Key & 0x7F) << 16 | (entry.Key & 0x80 ? 1 << 24 : 0), label, sizeof(label));
                if (*label)
                    result.Label = result.Key = label;
                result.Held |= key && (GetAsyncKeyState(key) & 0x8000) != 0;
            }
            else if (device == 1 && entry.Key < 5)
            {
                constexpr int keys[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
                const bool held = (GetAsyncKeyState(keys[entry.Key]) & 0x8000) != 0;
                if (held)
                    result.Label = fmt::format("Mouse {}", entry.Key + 1);
                result.Held |= held;
            }
            else if (device == 2 && getState)
            {
                for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
                {
                    XINPUT_STATE input{};
                    if (getState(index, &input) != ERROR_SUCCESS)
                        continue;
                    const bool held = entry.Key == 9 ? input.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD :
                        entry.Key == 10 ? input.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD :
                        (input.Gamepad.wButtons & entry.Key) != 0;
                    result.Held |= held;
                    result.Button = PadButtonName(entry.Key);
                    if (input.dwPacketNumber != s_lastPacket)
                    {
                        s_lastPacket = input.dwPacketNumber;
                        s_lastPadInput = GetTickCount64();
                    }
                    LASTINPUTINFO last{sizeof(LASTINPUTINFO)};
                    const uint64_t keyboardAge = GetLastInputInfo(&last) ? GetTickCount() - last.dwTime : UINT64_MAX;
                    result.Gamepad = s_lastPadInput && GetTickCount64() - s_lastPadInput < keyboardAge;
                    const char* label = entry.Key == XINPUT_GAMEPAD_A ? "A" : entry.Key == XINPUT_GAMEPAD_B ? "B" :
                        entry.Key == XINPUT_GAMEPAD_X ? "X" : entry.Key == XINPUT_GAMEPAD_Y ? "Y" :
                        entry.Key == 9 ? "LT" : entry.Key == 10 ? "RT" : "controller";
                    result.Label += std::string(" / ") + label;
                    break;
                }
            }
            break;
        }
    }
    return result;
}

ActivateInput ReadActivate() noexcept
{
    return ReadControl("Activate");
}

// Spectating: previous/next watched player. Mouse left/right, A/D or the arrow keys; D-pad left/right or LB/RB.
struct SpectateInput
{
    bool Previous{};
    bool Next{};
    bool Gamepad{};
};

SpectateInput ReadSpectate() noexcept
{
    SpectateInput result;
    const auto down = [](int aKey) { return (GetAsyncKeyState(aKey) & 0x8000) != 0; };
    result.Previous = down(VK_LBUTTON) || down('A') || down(VK_LEFT);
    result.Next = down(VK_RBUTTON) || down('D') || down(VK_RIGHT);
    using TXInput = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static TXInput getState = []() -> TXInput {
        for (auto* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (const auto module = LoadLibraryW(library))
                if (const auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<TXInput>(proc);
        return nullptr;
    }();
    for (DWORD index = 0; getState && index < XUSER_MAX_COUNT; ++index)
    {
        XINPUT_STATE input{};
        if (getState(index, &input) != ERROR_SUCCESS)
            continue;
        const auto buttons = input.Gamepad.wButtons;
        result.Previous |= (buttons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) != 0;
        result.Next |= (buttons & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) != 0;
        if (input.dwPacketNumber != s_lastPacket)
        {
            s_lastPacket = input.dwPacketNumber;
            s_lastPadInput = GetTickCount64();
        }
        LASTINPUTINFO last{sizeof(LASTINPUTINFO)};
        const uint64_t keyboardAge = GetLastInputInfo(&last) ? GetTickCount() - last.dwTime : UINT64_MAX;
        result.Gamepad = s_lastPadInput && GetTickCount64() - s_lastPadInput < keyboardAge;
        break;
    }
    return result;
}

// Vanilla conjuration visuals (Skyrim.esm): SummonTargetFX 3F811 (a summon's arrival), ReanimateFXShader 75272 (the
// spectral glow of a raised body), SummonMassCastBodyFX 44F57 (the caster's ritual aura).
constexpr uint32_t kArrivalArt = 0x3F811;
constexpr uint32_t kArrivalShader = 0x75272;
constexpr uint32_t kRitualArt = 0x44F57;
constexpr uint64_t kRitualMs = 2000;
constexpr uint64_t kMagickaLockMs = 5000;

// TESObjectREFR::ApplyArtObject 22769 / ApplyEffectShader 19872 (as ObjectReference/EffectShader.Play call them):
// (ref, form, duration, facingRef, facingArtObject, attachToCamera, attachNode, interfaceEffect).
void PlayArt(TESObjectREFR* aRef, uint32_t aForm, float aSeconds, bool aShader = false) noexcept
{
    using TApply = void*(TESObjectREFR*, TESForm*, float, TESObjectREFR*, bool, bool, NiAVObject*, bool);
    POINTER_SKYRIMSE(TApply, art, 22769);
    POINTER_SKYRIMSE(TApply, shader, 19872);
    auto* form = TESForm::GetById(aForm);
    if (!aRef || !form || !aRef->GetNiNode())
        return;
    (aShader ? shader : art).Get()(aRef, form, aSeconds, nullptr, false, false, nullptr, false);
}

// The third-person body: TESObjectREFR::Get3D (vfunc 0x380, 1402EBCE0) reads loadedState+0x68. GetNiNode returns the
// player's first-person body in first person (PlayerCharacter 140747AF0: +0x900).
NiAVObject* ThirdPerson3D(Actor* aActor) noexcept
{
    return aActor && aActor->loadedState ?
        *reinterpret_cast<NiAVObject**>(static_cast<uint8_t*>(aActor->loadedState) + 0x68) : nullptr;
}

// NiAVObject flag 1 (hidden) on the root culls the whole body; reapplied every frame, the game rebuilds 3D on loads.
void SetHidden(Actor* aActor, bool aHidden) noexcept
{
    const auto apply = [aHidden](NiAVObject* aRoot)
    {
        if (aRoot)
            aRoot->flags = aHidden ? (aRoot->flags | 1u) : (aRoot->flags & ~1u);
    };
    apply(ThirdPerson3D(aActor));
    if (aActor == PlayerCharacter::Get())
        apply(*reinterpret_cast<NiAVObject**>(reinterpret_cast<uint8_t*>(aActor) + 0x900));
}

// Game.SetCameraTarget (50838 / 1408FB250) writes PlayerCamera+0x3C; the third-person state follows that actor.
void SetCameraTarget(Actor* aActor) noexcept
{
    using TTarget = void(PlayerCamera*, Actor*);
    POINTER_SKYRIMSE(TTarget, target, 50838);
    if (auto* camera = PlayerCamera::Get())
        target.Get()(camera, aActor);
}

uint32_t CameraStateId() noexcept
{
    auto* camera = PlayerCamera::Get();
    return camera && camera->state ? camera->state->id : UINT32_MAX;
}

// The Shout key while the call-back ritual is armed (a fallen ally, this player out of combat with full magicka):
// its press is kept from ShoutHandler::ProcessButton (42430 / 1407B3960) so no shout starts; a 2 s hold performs
// the ritual. A tap is handed back as a press and release, so a quick one-word shout still works. Otherwise the key
// is untouched. ButtonEvent: value +0x28 (0 = released), heldDownSecs +0x2C (0 on the press).
struct ShoutButton
{
    uint8_t Pad[0x28];
    float Value;
    float Held;
};
static_assert(offsetof(ShoutButton, Held) == 0x2C);
using TShoutButton = void(void*, ShoutButton*);
TShoutButton* s_shoutButton = nullptr;
std::atomic<bool> s_ritualArmed{};
std::atomic<bool> s_shoutCapture{};
std::atomic<bool> s_ritualConsumed{};
std::atomic<bool> s_testShout{};
// The last lethal hit on the local player (GetTickCount64) and whether it was an overkill.
std::atomic<uint64_t> s_lethalAt{};
std::atomic<float> s_lethalShare{};
std::atomic<uint32_t> s_lethalAttacker{};
constexpr float kOverkillShare = 0.75f;
constexpr uint64_t kLethalWindowMs = 1500;
constexpr uint64_t kFlungRestMs = 3000;
constexpr uint64_t kFlungMaxMs = 10000;

void HookShoutButton(void* apHandler, ShoutButton* apEvent)
{
    const bool pressed = apEvent->Value != 0.f;
    if (!s_shoutCapture.load(std::memory_order_relaxed))
    {
        if (!pressed || apEvent->Held != 0.f || !s_ritualArmed.load(std::memory_order_relaxed))
        {
            s_shoutButton(apHandler, apEvent);
            return;
        }
        s_ritualConsumed.store(false, std::memory_order_relaxed);
        s_shoutCapture.store(true, std::memory_order_relaxed);
        return;
    }
    if (pressed)
        return;
    s_shoutCapture.store(false, std::memory_order_relaxed);
    if (!s_ritualConsumed.load(std::memory_order_relaxed) && apEvent->Held < 0.35f)
    {
        const float held = apEvent->Held;
        apEvent->Value = 1.f;
        apEvent->Held = 0.f;
        s_shoutButton(apHandler, apEvent);
        apEvent->Value = 0.f;
        apEvent->Held = held;
        s_shoutButton(apHandler, apEvent);
    }
}

TiltedPhoques::Initializer s_shoutHook([]()
{
    POINTER_SKYRIMSE(TShoutButton, button, 42430);
    s_shoutButton = button.Get();
    TP_HOOK(&s_shoutButton, HookShoutButton);
});

std::mutex s_testStateLock;
std::string s_testState{"{}"};
}

namespace
{
std::atomic<bool> s_testHold{};
// Test bridge: sets the local bleedout meter once while down (-1: nothing pending).
std::atomic<float> s_testBleed{-1.f};
}

void ReviveService::SetTestHold(bool aHeld) noexcept
{
    s_testHold.store(aHeld, std::memory_order_relaxed);
}

void ReviveService::SetTestBleed(float aBleed) noexcept
{
    s_testBleed.store(aBleed, std::memory_order_relaxed);
}

void ReviveService::NoteLethalHit(float aShareOfMax, uint32_t aAttacker) noexcept
{
    s_lethalShare.store(aShareOfMax, std::memory_order_relaxed);
    s_lethalAttacker.store(aAttacker, std::memory_order_relaxed);
    s_lethalAt.store(GetTickCount64(), std::memory_order_release);
}

void ReviveService::SetTestShout(bool aHeld) noexcept
{
    s_testShout.store(aHeld, std::memory_order_relaxed);
}

bool ReviveService::IsFallen(uint32_t aPlayerId) const noexcept
{
    if (aPlayerId == m_transport.GetLocalPlayerId())
        return m_fallen;
    const auto it = m_peers.find(aPlayerId);
    return it != m_peers.end() && it->second.Data.Dead;
}

std::string ReviveService::DescribeTest() noexcept
{
    std::lock_guard lock(s_testStateLock);
    return s_testState;
}

ReviveService::ReviveService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_notifyConnection(aDispatcher.sink<NotifyRevive>().connect<&ReviveService::OnNotify>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&ReviveService::OnDisconnected>(this))
{
}

std::string ReviveService::Name(uint32_t aId) const
{
    const auto& players = m_world.GetPartyService().GetPlayers();
    const auto it = players.find(aId);
    return it == players.end() ? "Player" : std::string(it->second.c_str());
}

bool ReviveService::IsPartyMember(uint32_t aId) const noexcept
{
    const auto& members = m_world.GetPartyService().GetPartyMembers();
    return std::find(members.begin(), members.end(), aId) != members.end();
}

Actor* ReviveService::FindPlayer(uint32_t aId) const noexcept
{
    if (aId == m_transport.GetLocalPlayerId())
        return PlayerCharacter::Get();
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
        if (view.get<PlayerComponent>(entity).Id == aId)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
    return nullptr;
}

void ReviveService::Reset(bool aRestorePlayer) noexcept
{
    if (m_fallen && aRestorePlayer)
        if (auto* player = PlayerCharacter::Get())
            LeaveFallen(player, nullptr);
    m_fallen = m_flung = false;
    m_dyingSince = 0;
    m_watch = m_cameraOn = 0;
    s_ritualArmed.store(false, std::memory_order_relaxed);
    m_wiped = false;
    CancelHold();
    for (const auto& [id, peer] : m_peers)
        if (aRestorePlayer && (peer.AppliedDown || peer.AppliedDead))
            if (auto* actor = FindPlayer(id))
            {
                actor->SetNoBleedoutRecovery(false);
                SetHidden(actor, false);
            }
    m_peers.clear();
    m_active = m_down = false;
    m_nextState = m_nextNotice = 0;
    m_revivedBy = 0;
    PushUi(0, {}, {}, 0, {}, GetTickCount64());
    // Revision stays monotonic across party changes on this connection.
}

void ReviveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    if (m_down)
        if (auto* player = PlayerCharacter::Get())
        {
            Recover(player, 0.5f);
            player->SetNoBleedoutRecovery(false);
        }
    Reset();
}

void ReviveService::SendState(PlayerCharacter* aPlayer, uint64_t aNow) noexcept
{
    ReviveRequest request;
    request.Epoch = m_epoch;
    request.Revision = m_revision;
    request.Down = m_down;
    request.Alive = !aPlayer->actorState.IsDeadState() && aPlayer->GetActorValue(ActorValueInfo::kHealth) > 0.f;
    request.InCombat = aPlayer->IsInCombat();
    request.Bleed = m_down ? m_bleed : m_fallen ? 0.f : 1.f;
    request.Dead = m_fallen;
    request.Flung = m_fallen && m_flung;
    m_alive = request.Alive;
    m_combat = request.InCombat;
    m_transport.Send(request);
    m_nextState = aNow + 1000;
}

void ReviveService::SendHold(ReviveAction aAction) noexcept
{
    ReviveRequest request;
    request.Action = aAction;
    request.Epoch = m_epoch;
    request.PlayerId = m_holdTarget;
    request.Revision = m_holdRevision;
    m_transport.Send(request);
}

void ReviveService::CancelHold() noexcept
{
    if (m_holdSince && m_transport.IsConnected())
        SendHold(ReviveAction::Cancel);
    m_holdSince = m_nextHold = 0;
    m_holdTarget = 0;
}

bool ReviveService::Near(PlayerCharacter* aPlayer, const Peer& aPeer, float aRadius) const noexcept
{
    GameId cell, worldspace;
    auto* parent = aPlayer->GetParentCellEx();
    if (!parent || !m_world.GetModSystem().GetServerModId(parent->formID, cell))
        return false;
    if (parent->worldspace && !m_world.GetModSystem().GetServerModId(parent->worldspace->formID, worldspace))
        return false;
    if (worldspace != aPeer.Data.WorldSpace || (!worldspace && cell != aPeer.Data.Cell))
        return false;
    const auto delta = aPlayer->position - static_cast<const glm::vec3&>(aPeer.Data.Position);
    const float distance = glm::dot(delta, delta);
    return std::isfinite(distance) && distance <= aRadius * aRadius;
}

void ReviveService::RestoreSpells(PlayerCharacter* aPlayer) noexcept
{
    auto* equip = EquipManager::Get();
    if (auto* spell = TESForm::GetById(m_mainSpell))
        equip->EquipSpell(aPlayer, spell, 0);
    if (auto* spell = TESForm::GetById(m_secondarySpell))
        equip->EquipSpell(aPlayer, spell, 1);
    if (auto* power = TESForm::GetById(m_power))
        equip->EquipShout(aPlayer, power);
}

bool ReviveService::CombatAround(PlayerCharacter* aPlayer, uint32_t aOther) const noexcept
{
    if (aPlayer && aPlayer->IsInCombat())
        return true;
    for (const auto& [id, peer] : m_peers)
        if (peer.Data.Alive && peer.Data.InCombat && (id == aOther || (aPlayer && Near(aPlayer, peer, 2000.f))))
            return true;
    return false;
}

void ReviveService::PushUi(int aMode, const std::string& acName, const std::string& acKey, double aProgress,
    const std::string& acHint, uint64_t aNow, const std::string& acNote, const std::string& acButton, bool aGamepad,
    float aBleed) noexcept
{
    if (aNow >= m_nextModel)
    {
        m_model = ControlBindings::ConnectedControllerModel();
        m_nextModel = aNow + 5000;
    }
    const bool changed = aMode != m_uiMode || acName != m_uiName || acKey != m_uiKey || acHint != m_uiHint ||
        acNote != m_uiNote || acButton != m_uiButton || aGamepad != m_uiGamepad || m_model != m_uiModel;
    // The bleedout meter moves 1/300 per second: redraw on each 1/1000 step (and the m:ss display each second).
    const bool progressed = (std::abs(aProgress - m_uiProgress) >= 0.01 || std::abs(aBleed - m_uiBleed) >= 0.001f) &&
        aNow - m_uiSent >= 50;
    if (!changed && !progressed)
        return;
    if (changed)
        spdlog::info("Revive overlay: mode {} name '{}' key '{}' hint '{}'", aMode, acName, acKey, acHint);
    m_uiMode = aMode;
    m_uiName = acName;
    m_uiKey = acKey;
    m_uiHint = acHint;
    m_uiNote = acNote;
    m_uiButton = acButton;
    m_uiGamepad = aGamepad;
    m_uiModel = m_model;
    m_uiProgress = aProgress;
    m_uiBleed = aBleed;
    m_uiSent = aNow;
    m_world.GetOverlayService().PushRevive(aMode, acName, acKey, aProgress, acHint, acNote, acButton, m_model, aGamepad, aBleed);
}

void ReviveService::OnNotify(const NotifyRevive& aMessage) noexcept
{
    if (aMessage.Action == ReviveAction::Hold || aMessage.Action == ReviveAction::Cancel)
    {
        static std::atomic<uint32_t> s_logs{};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 20)
            spdlog::info("Revive: notice action {} player {} reviver {} epoch {} (mine {}) valid {} active {} member {}",
                static_cast<unsigned>(aMessage.Action), aMessage.PlayerId, aMessage.ReviverId, aMessage.Epoch, m_epoch,
                aMessage.IsValid(), m_active, IsPartyMember(aMessage.PlayerId));
    }
    if (aMessage.IsValid() && m_active && aMessage.Epoch == m_epoch && aMessage.Action == ReviveAction::Wipe && !m_wiped)
    {
        // Nobody is standing: everyone collapses like a vanilla death (ObjectReference.PushActorAway with no
        // force: AIProcess::KnockExplosion 39895); the server then reloads the party's checkpoint.
        m_wiped = true;
        CancelHold();
        s_ritualArmed.store(false, std::memory_order_relaxed);
        if (auto* player = PlayerCharacter::Get(); player && player->currentProcess && !m_fallen)
        {
            using TKnock = void(AIProcess*, Actor*, const NiPoint3&, float);
            POINTER_SKYRIMSE(TKnock, knock, 39895);
            knock.Get()(player->currentProcess, player, player->position, 0.f);
        }
        spdlog::info("Revive: party wiped");
        Notice("All have fallen");
        PushUi(6, {}, {}, 0, "Returning to your last save", GetTickCount64());
        return;
    }
    if (!aMessage.IsValid() || !m_active || aMessage.Epoch != m_epoch || !IsPartyMember(aMessage.PlayerId))
        return;
    if (aMessage.Action == ReviveAction::Grant)
    {
        auto* player = PlayerCharacter::Get();
        if (aMessage.PlayerId != m_transport.GetLocalPlayerId() || !m_down || m_fallen || !player ||
            aMessage.Revision != m_revision || !IsPartyMember(aMessage.ReviverId))
            return;
        const auto reviver = m_peers.find(aMessage.ReviverId);
        if (reviver == m_peers.end() || GetTickCount64() - reviver->second.Received > 3500 ||
            reviver->second.Data.Down || !reviver->second.Data.Alive || reviver->second.Data.InCombat ||
            !Near(player, reviver->second, 200.f))
            return;
        // Health comes back in proportion to the bleedout meter left (owner design, 2026-09-28), at least 5%.
        const float restored = (std::max)(0.05f, m_bleed);
        Recover(player, restored);
        spdlog::info("Revive: restored {:.0f}% health (bleedout meter {:.3f})", restored * 100.f, m_bleed);
        m_down = false;
        m_revivedBy = 0;
        ++m_revision;
        m_nextNotice = 0;
        RestoreSpells(player);
        spdlog::info("Revive: revived by {}", Name(aMessage.ReviverId));
        Notice(fmt::format("Revived by {}", Name(aMessage.ReviverId)));
        SendState(player, GetTickCount64());
        return;
    }
    if (aMessage.PlayerId == m_transport.GetLocalPlayerId())
    {
        auto* player = PlayerCharacter::Get();
        if (aMessage.Action == ReviveAction::Raise)
        {
            spdlog::info("Revive: raise notice from {} (fallen {}, wiped {})", aMessage.ReviverId ? Name(aMessage.ReviverId) :
                std::string("the server"), m_fallen, m_wiped);
            m_wiped = false;
            if (m_fallen && player)
                LeaveFallen(player, &aMessage);
            else if (m_down && player)
            {
                // A wipe with no checkpoint to reload: everyone gets back up where they fell.
                Recover(player, 1.f);
                m_down = false;
                ++m_revision;
                RestoreSpells(player);
                SendState(player, GetTickCount64());
            }
            PushUi(0, {}, {}, 0, {}, GetTickCount64());
            return;
        }
        // The server keeps a fallen player fallen: after a reload or reconnect this player comes back fallen.
        if (aMessage.Action == ReviveAction::State && aMessage.Dead && !m_fallen && player)
        {
            EnterFallen(player, "the server has this player fallen");
            return;
        }
        if (aMessage.Action == ReviveAction::Hold && aMessage.ReviverId != m_revivedBy)
            spdlog::info("Revive: hold notice from {} (down {}, party member {})", Name(aMessage.ReviverId), m_down,
                IsPartyMember(aMessage.ReviverId));
        // Someone is holding the revive on this player (forwarded by the server every 250 ms), or stopped.
        if (m_down && aMessage.Action == ReviveAction::Hold && IsPartyMember(aMessage.ReviverId))
        {
            const auto now = GetTickCount64();
            if (m_revivedBy != aMessage.ReviverId || now - m_revivedLast > 1000)
                m_revivedSince = now;
            m_revivedBy = aMessage.ReviverId;
            m_revivedLast = now;
        }
        else if (aMessage.Action == ReviveAction::Cancel && aMessage.ReviverId == m_revivedBy)
            m_revivedBy = 0;
        return;
    }
    auto& peer = m_peers[aMessage.PlayerId];
    if (aMessage.Revision < peer.Data.Revision)
        return;
    if (aMessage.Dead && !peer.Data.Dead)
    {
        Notice(aMessage.Flung ? fmt::format("{} has been slain", Name(aMessage.PlayerId)) :
            fmt::format("{} has fallen", Name(aMessage.PlayerId)));
        spdlog::info("Revive: {} fallen", Name(aMessage.PlayerId));
    }
    else if (!aMessage.Dead && peer.Data.Dead && peer.Received)
    {
        Notice(fmt::format("{} has returned", Name(aMessage.PlayerId)));
        spdlog::info("Revive: {} returned", Name(aMessage.PlayerId));
    }
    if (aMessage.Down && !aMessage.Dead &&
        (!peer.Received || !peer.Data.Down || aMessage.Revision != peer.Data.Revision))
    {
        Notice(fmt::format("{} is down", Name(aMessage.PlayerId)));
        spdlog::info("Revive: {} down", Name(aMessage.PlayerId));
    }
    peer.Data = aMessage;
    peer.Received = GetTickCount64();
}

void ReviveService::ApplyPeers() noexcept
{
    for (auto& [id, peer] : m_peers)
    {
        auto* actor = FindPlayer(id);
        if (!actor || !actor->GetNiNode() || actor->actorState.IsDeadState())
        {
            peer.AppliedForm = 0;
            continue;
        }
        if (peer.AppliedForm == actor->formID && peer.AppliedRevision == peer.Data.Revision &&
            peer.AppliedDead == peer.Data.Dead && peer.AppliedFlung == peer.Data.Flung)
        {
            if (peer.Data.Dead && !peer.Data.Flung)
                SetHidden(actor, true);
            continue;
        }
        spdlog::info("Revive: apply {} to {:X} (revision {}, form was {:X}, life {})",
            peer.Data.Dead ? "fallen" : peer.Data.Down ? "down" : "up",
            actor->formID, peer.Data.Revision, peer.AppliedForm, (actor->actorState.flags1 >> 21) & 0xF);
        if (peer.Data.Dead)
        {
            // A fallen player: hidden and in essential down (never a combat target) until called back. A flung one
            // keeps its body (the owner's ragdoll streams in) until the owner reports it at rest.
            actor->SetNoBleedoutRecovery(true);
            if (((actor->actorState.flags1 >> 21) & 0xF) != 7)
            {
                LifeState(actor, 7);
                if (!peer.Data.Flung)
                    PlayRemoteBleedout(actor, true);
            }
            SetHidden(actor, !peer.Data.Flung);
        }
        else if (peer.Data.Down)
        {
            actor->SetNoBleedoutRecovery(true);
            LifeState(actor, 7);
            const bool played = PlayRemoteBleedout(actor, true);
            spdlog::info("Revive: {:X} bleedout start action played {}", actor->formID, played);
        }
        else if (peer.AppliedDown || peer.AppliedDead || actor->actorState.IsBleedingOut())
        {
            SetHidden(actor, false);
            if (peer.AppliedDead)
            {
                PlayArt(actor, kArrivalArt, 3.f);
                PlayArt(actor, kArrivalShader, 3.f, true);
            }
            // Health is replicated by the owner's actor-value stream, including full-health fallbacks.
            actor->SetNoBleedoutRecovery(false);
            // The stop idle is chosen while the copy is still in bleedout (its conditions see that state).
            const bool played = PlayRemoteBleedout(actor, false);
            LifeState(actor, 0);
            // If the stop idle is refused, the copy's graph stays in bleedout under the owner's streamed pose and
            // shows whenever that stream gaps: reset the graph to its default (standing) state.
            bool reset = false;
            if (!played)
            {
                BSFixedString idle("IdleForceDefaultState");
                reset = actor->SendAnimationEvent(&idle);
            }
            spdlog::info("Revive: {:X} bleedout stop action played {}, default-state reset {}", actor->formID, played, reset);
        }
        peer.AppliedForm = actor->formID;
        peer.AppliedRevision = peer.Data.Revision;
        peer.AppliedDown = peer.Data.Down;
        peer.AppliedDead = peer.Data.Dead;
        peer.AppliedFlung = peer.Data.Flung;
    }
}

bool ReviveService::Update(bool aEnabled) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!aEnabled || !m_transport.IsConnected() || !party.IsInParty())
    {
        if (m_active)
            Reset();
        return false;
    }
    if (!m_active || m_epoch != party.GetStartEpoch())
    {
        // A new epoch is a (re)start: its load replaces the player, which must not be touched here.
        Reset(!m_active);
        m_epoch = party.GetStartEpoch();
        m_active = true;
        ++m_revision;
    }
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    if (!player || !ui || !player->parentCell || !player->GetNiNode() ||
        ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu")))
    {
        PushUi(0, {}, {}, 0, {}, GetTickCount64());
        CancelHold();
        return true;
    }
    const auto now = GetTickCount64();
    // Knock state changes of the local player (fling measurement): ragdoll, get up.
    {
        static uint32_t s_knock{};
        const uint32_t knock = (player->actorState.flags1 >> 25) & 0x7;
        if (knock != s_knock)
        {
            spdlog::info("Player knock state {} -> {} (life {}, health {:.1f})", s_knock, knock,
                (player->actorState.flags1 >> 21) & 0xF, player->GetActorValue(ActorValueInfo::kHealth));
            s_knock = knock;
        }
    }
    // Owner rule (2026-09-28): being flung (a giant's club, a mammoth, Unrelenting Force) or an overkill blow kills
    // outright, as in vanilla: no bleedout. Vanilla's famous launch is the death ragdoll taking the blow's impulse,
    // which an essential player never gets (giant test: no knock state after a 478-damage club hit). So an overkill
    // blow knocks the player away from the attacker, scaled by the blow (ObjectReference.PushActorAway's
    // AIProcess::KnockExplosion 39895); a lethal hit that knocks the player down by itself counts too. The player is
    // left alone while it flies (entering essential down at once made the host slide in the bleedout pose) and is
    // fallen once the body is nearly still (owner: a giant's downswing launches players sky-high for 20 s or more),
    // or at once when the player presses Activate to skip to spectating.
    if (!m_fallen && !m_wiped)
    {
        const bool knocked = ((player->actorState.flags1 >> 25) & 0x7) != 0;
        if (!m_dyingSince)
            if (const auto lethal = s_lethalAt.exchange(0, std::memory_order_acq_rel); lethal && now - lethal <= kLethalWindowMs)
            {
                const float share = s_lethalShare.load(std::memory_order_relaxed);
                m_dyingSince = now;
                m_dyingOverkill = share >= kOverkillShare;
                m_dyingKnocked = false;
                m_dyingKnockSince = 0;
                m_dyingCollapsed = false;
                m_dyingLastPos = player->position;
                m_dyingLastSample = now;
                m_dyingStillSince = 0;
                m_skipHeld = true;
                if (m_dyingOverkill && player->currentProcess)
                {
                    NiPoint3 origin = player->position;
                    if (auto* attacker = Cast<Actor>(TESForm::GetById(s_lethalAttacker.load(std::memory_order_relaxed))))
                        origin = attacker->position;
                    const float magnitude = std::clamp(share * 6.f, 3.f, 30.f);
                    using TKnock = void(AIProcess*, Actor*, const NiPoint3&, float);
                    POINTER_SKYRIMSE(TKnock, knock, 39895);
                    knock.Get()(player->currentProcess, player, origin, magnitude);
                    m_dyingCollapsed = true;
                    spdlog::info("Revive: overkill ({:.0f}% of max health), knocked away with magnitude {:.1f}",
                        share * 100.f, magnitude);
                }
                else
                    spdlog::info("Revive: lethal hit ({:.0f}% of max health, knocked {})", share * 100.f, knocked);
            }
        if (m_dyingSince)
        {
            if (knocked && !m_dyingKnocked)
            {
                m_dyingKnocked = true;
                m_dyingKnockSince = now;
            }
            const auto elapsed = now - m_dyingSince;
            // Nearly still: under 15 u moved per 250 ms sample, held for a second.
            if (now - m_dyingLastSample >= 250)
            {
                const auto delta = player->position - m_dyingLastPos;
                const bool still = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) < 15.f;
                m_dyingStillSince = still ? (m_dyingStillSince ? m_dyingStillSince : now) : 0;
                m_dyingLastPos = player->position;
                m_dyingLastSample = now;
            }
            const auto skip = ReadActivate();
            const bool skipPressed = skip.Held && !m_skipHeld;
            m_skipHeld = skip.Held;
            bool fall = false;
            if (m_dyingKnocked || m_dyingCollapsed)
            {
                if (skipPressed)
                    fall = true;
                else if (m_dyingStillSince && now - m_dyingStillSince >= 1000 && elapsed >= 1500)
                    fall = true; // came to rest
                else if (elapsed >= 25000)
                    fall = true;
                else
                    PushUi(4, {}, {}, 0, fmt::format("{}: watch your allies", skip.Key.empty() ? skip.Label : skip.Key),
                        now, "Slain", skip.Button, skip.Gamepad);
            }
            else if (elapsed >= kLethalWindowMs)
                m_dyingSince = 0; // no fling and no overkill: an ordinary down
            if (fall)
            {
                // At rest (or skipped): straight to spectating; the body is hidden like any fallen player's.
                m_dyingSince = 0;
                m_flung = false;
                EnterFallen(player, m_dyingCollapsed ? "overkill" : "flung");
                return true;
            }
            if (m_dyingSince)
            {
                if (!m_dyingKnocked && !m_dyingCollapsed)
                    PushUi(0, {}, {}, 0, {}, now);
                return true;
            }
        }
    }
    // A fallen player stays in essential down (IsBleedingOut) while spectating; that is not a new down.
    const bool down = !m_fallen && player->actorState.IsBleedingOut();
    if (down != m_down)
    {
        m_down = down;
        ++m_revision;
        m_nextState = 0;
        CancelHold();
        if (down)
        {
            player->SetNoBleedoutRecovery(true);
            // Owner rule (2026-09-28): downed players are never combat targets. The native target check
            // (14081CCE0 via Actor vfunc 0x4C8, 1406879A0) rejects life states 1, 2, 5 and 7 (essential down) but
            // not 8 (bleedout), which the engine gives an essential player. 8 -> 7 stays inside bleedout in
            // SetLifeState (37612: no camera or control change); remote copies are already put in 7 (ApplyPeers).
            if (((player->actorState.flags1 >> 21) & 0xF) == 8)
                LifeState(player, 7);
            // One notice for the HUD log; the overlay shows the state while down.
            Notice("You are down. A nearby ally can revive you before you bleed out.");
            m_revivedBy = 0;
            m_bleed = 1.f;
            m_bleedTick = now;
            spdlog::info("Revive: {} down", Name(m_transport.GetLocalPlayerId()));
            m_nextNotice = now + 6000;
        }
    }
    const bool combat = player->IsInCombat();
    const bool alive = !player->actorState.IsDeadState() && player->GetActorValue(ActorValueInfo::kHealth) > 0.f;
    if (now >= m_nextState || combat != m_combat || alive != m_alive)
        SendState(player, now);
    std::erase_if(m_peers, [&](const auto& entry) {
        const bool expired = !IsPartyMember(entry.first) || now - entry.second.Received > 3500;
        if (expired && (entry.second.AppliedDown || entry.second.AppliedDead))
            if (auto* actor = FindPlayer(entry.first))
            {
                actor->SetNoBleedoutRecovery(false);
                SetHidden(actor, false);
            }
        return expired;
    });
    ApplyPeers();
    const bool paused = ui->numPausesGame || ui->numItemMenus || ui->modal ||
        ui->GetMenuOpen(BSFixedString("Console")) || m_world.GetOverlayService().GetActive();
    auto* window = BSGraphics::GetMainWindow();
    const bool input = !paused && window && GetForegroundWindow() == window->hWnd &&
        !ControlBindings::IsCapturing() && (m_down || m_fallen || PlayerCollision::LocalHasFreeControl()) &&
        !ui->GetMenuOpen(BSFixedString("Dialogue Menu"));
    {
        std::lock_guard lock(s_testStateLock);
        s_testState = fmt::format("{{\"fallen\":{},\"down\":{},\"watch\":{},\"watchName\":\"{}\",\"cameraState\":{},"
            "\"life\":{},\"hidden\":{},\"ritualArmed\":{},\"ritualTarget\":{},\"magicka\":{:.1f},\"bleed\":{:.3f}}}",
            m_fallen ? "true" : "false", m_down ? "true" : "false", m_watch, m_watch ? Name(m_watch) : std::string(),
            CameraStateId(), (player->actorState.flags1 >> 21) & 0xF,
            ThirdPerson3D(player) && (ThirdPerson3D(player)->flags & 1u) ? "true" : "false",
            s_ritualArmed.load() ? "true" : "false", m_ritualTarget, player->GetActorValue(ActorValueInfo::kMagicka), m_bleed);
    }
    if (m_wiped)
    {
        PushUi(6, {}, {}, 0, "Returning to your last save", now);
        return true;
    }
    if (paused)
    {
        CancelHold();
        s_ritualArmed.store(false, std::memory_order_relaxed);
        PushUi(0, {}, {}, 0, {}, now);
        return true;
    }
    if (m_fallen)
    {
        UpdateFallen(player, now, input);
        return true;
    }
    if (m_down)
    {
        s_ritualArmed.store(false, std::memory_order_relaxed);
        {
            // Being revived: the reviver's name and the 3 s hold (stale after 700 ms without a notice).
            const bool revived = m_revivedBy && now - m_revivedLast <= 700;
            // The bleedout meter drains in real time and holds still while someone revives this player.
            if (m_bleedTick && !revived)
                m_bleed -= static_cast<float>(now - m_bleedTick) / static_cast<float>(kBleedoutMs);
            m_bleedTick = now;
            if (const float forced = s_testBleed.exchange(-1.f, std::memory_order_relaxed); forced >= 0.f)
                m_bleed = forced;
            if (m_bleed <= 0.f)
            {
                // Bled out: fallen until an ally calls this player back (owner design 2026-09-28; no respawn).
                m_bleed = 0.f;
                EnterFallen(player, "bled out");
                return true;
            }
            const bool slow = revived && CombatAround(player, m_revivedBy);
            PushUi(3, revived ? Name(m_revivedBy) : std::string(), {}, revived ?
                (std::min)(1.0, static_cast<double>(now - m_revivedSince) / (slow ? 6000.0 : 3000.0)) : 0.0,
                {}, now, slow ? "In combat: reviving takes longer" : std::string(), {}, false, m_bleed);
        }
        return true;
    }
    m_mainSpell = player->magicItems[0] ? player->magicItems[0]->formID : 0;
    m_secondarySpell = player->magicItems[1] ? player->magicItems[1]->formID : 0;
    m_power = player->equippedShout ? player->equippedShout->formID : 0;
    const bool ritual = UpdateRitual(player, now, input && alive);
    if (!input || !alive)
    {
        CancelHold();
        if (!ritual)
            PushUi(0, {}, {}, 0, {}, now);
        return true;
    }
    auto activate = ReadActivate();
    activate.Held |= s_testHold.load(std::memory_order_relaxed);
    uint32_t target{};
    uint64_t revision{};
    float closest = 200.f * 200.f;
    for (const auto& [id, peer] : m_peers)
    {
        if (!peer.Data.Down || peer.Data.Dead || !Near(player, peer, 250.f))
            continue;
        auto* actor = FindPlayer(id);
        if (!actor || !actor->GetNiNode() || actor->IsDisabled() || actor->IsDeleted())
            continue;
        const auto delta = actor->position - player->position;
        const float distance = glm::dot(delta, delta);
        const float facing = std::sin(player->rotation.z) * delta.x + std::cos(player->rotation.z) * delta.y;
        if (!std::isfinite(distance) || distance > closest || facing < 0.5f * std::sqrt(delta.x * delta.x + delta.y * delta.y))
            continue;
        target = id;
        revision = peer.Data.Revision;
        closest = distance;
    }
    const bool slow = target && CombatAround(player, target);
    const std::string note = slow ? "In combat: reviving takes longer" : std::string();
    if (!target || !activate.Held)
    {
        CancelHold();
        // The activation prompt under the crosshair: "[key] Revive" below the downed player's name.
        if (target)
            PushUi(1, Name(target), activate.Key.empty() ? activate.Label : activate.Key, 0, "Revive", now, note,
                activate.Button, activate.Gamepad, m_peers[target].Data.Bleed);
        else if (!ritual)
            PushUi(0, {}, {}, 0, {}, now);
        return true;
    }
    if (!m_holdSince || m_holdTarget != target || m_holdRevision != revision)
    {
        CancelHold();
        m_holdTarget = target;
        m_holdRevision = revision;
        m_holdSince = now;
        m_nextHold = 0;
        spdlog::info("Revive: revive started by {}", Name(m_transport.GetLocalPlayerId()));
    }
    const uint64_t required = slow ? 6000 : 3000;
    if (now >= m_nextHold)
    {
        SendHold(ReviveAction::Hold);
        if (now - m_holdSince >= required)
            SendHold(ReviveAction::Finish);
        m_nextHold = now + 250;
    }
    PushUi(2, Name(target), {}, (std::min)(1.0, static_cast<double>(now - m_holdSince) / static_cast<double>(required)), {},
        now, note, {}, false, m_peers[target].Data.Bleed);
    return true;
}

TESObjectCELL* ReviveService::PeerCell(const ReviveData& aData, const NiPoint3& aPosition) const noexcept
{
    auto& mods = m_world.GetModSystem();
    if (aData.WorldSpace)
    {
        // Exterior cells can be created at runtime (no shared id): resolve by worldspace and coordinates.
        auto* worldSpace = Cast<TESWorldSpace>(TESForm::GetById(mods.GetGameId(aData.WorldSpace)));
        if (!worldSpace)
            return nullptr;
        return ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(aPosition.x / 4096.f)),
            static_cast<int32_t>(std::floor(aPosition.y / 4096.f)), worldSpace, true);
    }
    return Cast<TESObjectCELL>(TESForm::GetById(mods.GetGameId(aData.Cell)));
}

void ReviveService::EnterFallen(PlayerCharacter* aPlayer, const char* aReason) noexcept
{
    m_fallen = true;
    m_down = false;
    m_revivedBy = 0;
    m_bleed = 0.f;
    m_watch = m_cameraOn = 0;
    m_nextCamera = m_nextWatchMove = 0;
    m_prevHeld = m_nextHeld = true;
    m_wasFirstPerson = CameraStateId() == 0;
    CancelHold();
    s_ritualArmed.store(false, std::memory_order_relaxed);
    aPlayer->SetNoBleedoutRecovery(true);
    if (((aPlayer->actorState.flags1 >> 21) & 0xF) != 7)
        LifeState(aPlayer, 7);
    if (!m_flung)
        SetHidden(aPlayer, true);
    ++m_revision;
    spdlog::info("Revive: fallen ({})", aReason);
    Notice(m_flung ? "You have been slain. An ally can call you back." : "You have fallen. An ally can call you back.");
    if (m_transport.IsConnected())
        SendState(aPlayer, GetTickCount64());
}

void ReviveService::LeaveFallen(PlayerCharacter* aPlayer, const NotifyRevive* apRaise) noexcept
{
    m_fallen = false;
    m_flung = false;
    m_watch = m_cameraOn = 0;
    SetHidden(aPlayer, false);
    SetCameraTarget(aPlayer);
    if (apRaise)
    {
        // Beside the caster (its position rides in the raise notice).
        NiPoint3 at = static_cast<const glm::vec3&>(apRaise->Position);
        at.x += 90.f;
        at.y += 90.f;
        if (auto* cell = PeerCell(*apRaise, at))
            aPlayer->MoveTo(cell, at);
    }
    // Leaves essential down through SetLifeState: ends the bleedout camera and restores the stored controls.
    Recover(aPlayer, apRaise ? 0.5f : 1.f);
    if (auto* camera = PlayerCamera::Get(); camera && camera->state)
    {
        if (m_wasFirstPerson && camera->state->id != 0)
            camera->ForceFirstPerson();
        else if (camera->state->id != 0 && camera->state->id != 9)
            camera->ForceThirdPerson();
    }
    RestoreSpells(aPlayer);
    m_bleed = 1.f;
    m_down = false;
    ++m_revision;
    if (apRaise)
    {
        PlayArt(aPlayer, kArrivalArt, 3.f);
        PlayArt(aPlayer, kArrivalShader, 3.f, true);
        Notice(apRaise->ReviverId ? fmt::format("{} called you back", Name(apRaise->ReviverId)) : std::string("You rise again"));
        spdlog::info("Revive: called back by {}", apRaise->ReviverId ? Name(apRaise->ReviverId) : std::string("the server"));
    }
    else
        spdlog::info("Revive: fallen state cleared locally");
    if (m_transport.IsConnected())
        SendState(aPlayer, GetTickCount64());
}

void ReviveService::UpdateFallen(PlayerCharacter* aPlayer, uint64_t aNow, bool aInput) noexcept
{
    if (m_flung)
    {
        // The camera stays on the landed body a moment (the knock state stays set in essential down, so time it).
        if (!m_flungRestSince)
            m_flungRestSince = aNow;
        if ((m_flungRestSince && aNow - m_flungRestSince >= kFlungRestMs) || aNow - m_flungSince >= kFlungMaxMs)
        {
            m_flung = false;
            ++m_revision;
            SendState(aPlayer, aNow);
            spdlog::info("Revive: flung body at rest, spectating");
        }
        else
        {
            PushUi(4, {}, {}, 0, "An ally can call you back", aNow, "Slain");
            return;
        }
    }
    // Reapplied every frame: a save load or cell change brings the body and the alive state back.
    SetHidden(aPlayer, true);
    aPlayer->SetNoBleedoutRecovery(true);
    if (((aPlayer->actorState.flags1 >> 21) & 0xF) != 7)
        LifeState(aPlayer, 7);
    std::vector<uint32_t> living;
    for (const auto& [id, peer] : m_peers)
        if (IsPartyMember(id) && peer.Data.Alive && !peer.Data.Dead)
            living.push_back(id);
    const auto spectate = aInput ? ReadSpectate() : SpectateInput{};
    if (living.empty())
    {
        if (m_cameraOn)
        {
            SetCameraTarget(aPlayer);
            m_cameraOn = 0;
        }
        m_watch = 0;
        PushUi(4, {}, "A|D", 0, "An ally can call you back", aNow, "No allies are standing", {}, spectate.Gamepad);
        return;
    }
    auto current = std::find(living.begin(), living.end(), m_watch);
    if (current == living.end())
        current = living.begin();
    const auto count = static_cast<std::ptrdiff_t>(living.size());
    auto index = current - living.begin();
    if (spectate.Previous && !m_prevHeld)
        index = (index + count - 1) % count;
    if (spectate.Next && !m_nextHeld)
        index = (index + 1) % count;
    m_prevHeld = spectate.Previous;
    m_nextHeld = spectate.Next;
    const auto watch = living[static_cast<size_t>(index)];
    if (watch != m_watch)
    {
        spdlog::info("Revive: spectating {}", Name(watch));
        m_watch = watch;
        m_nextWatchMove = 0;
    }
    const auto& peer = m_peers[watch];
    auto* actor = FindPlayer(watch);
    // The loaded area follows this player, not the camera: keep the hidden body near the watched player.
    if ((!actor || !actor->GetNiNode() || !Near(aPlayer, peer, 4000.f)) && aNow >= m_nextWatchMove)
    {
        NiPoint3 at = static_cast<const glm::vec3&>(peer.Data.Position);
        at.x -= 150.f;
        if (auto* cell = PeerCell(peer.Data, at))
        {
            aPlayer->MoveTo(cell, at);
            spdlog::info("Revive: spectator moved next to {}", Name(watch));
        }
        m_nextWatchMove = aNow + 3000;
    }
    if (actor && actor->GetNiNode() && (m_cameraOn != watch || aNow >= m_nextCamera))
    {
        SetCameraTarget(actor);
        if (auto* camera = PlayerCamera::Get(); camera && CameraStateId() != 9)
        {
            // Essential down starts the bleedout camera (state 11), which refuses the forced views: end it first
            // (PlayerCamera::StopBleedoutCamera 50824 / 1408F9EB0, instant), as SetLifeState does on recovery.
            if (CameraStateId() == 11)
            {
                using TStop = void(PlayerCamera*, bool);
                POINTER_SKYRIMSE(TStop, stop, 50824);
                stop.Get()(camera, true);
            }
            camera->ForceFirstPerson();
            camera->ForceThirdPerson();
        }
        if (m_cameraOn != watch)
            spdlog::info("Revive: camera on {} (state {})", Name(watch), CameraStateId());
        m_cameraOn = watch;
        m_nextCamera = aNow + 500;
    }
    PushUi(4, Name(watch), "A|D", 0, "An ally can call you back", aNow,
        count > 1 ? fmt::format("{} of {}", index + 1, count) : std::string(), {}, spectate.Gamepad);
}

bool ReviveService::UpdateRitual(PlayerCharacter* aPlayer, uint64_t aNow, bool aInput) noexcept
{
    // Magicka stays spent for a few seconds after the ritual.
    if (m_magickaLockUntil)
    {
        if (aNow < m_magickaLockUntil)
            aPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kMagicka, 0.f);
        else
            m_magickaLockUntil = 0;
    }
    uint32_t fallen = 0;
    for (const auto& [id, peer] : m_peers)
        if (peer.Data.Dead && IsPartyMember(id))
        {
            fallen = id;
            break;
        }
    if (!fallen || !aInput || m_down || m_fallen)
    {
        s_ritualArmed.store(false, std::memory_order_relaxed);
        m_ritualTarget = 0;
        m_ritualSince = 0;
        return false;
    }
    const float magicka = aPlayer->GetActorValue(ActorValueInfo::kMagicka);
    const float maximum = magicka - aPlayer->magickaModifiers.damageModifier;
    const bool full = maximum > 0.f && magicka >= maximum - 0.5f;
    const bool combat = CombatAround(aPlayer, 0);
    const bool armed = full && !combat && !m_magickaLockUntil;
    s_ritualArmed.store(armed, std::memory_order_relaxed);
    const bool held = armed && (s_shoutCapture.load(std::memory_order_relaxed) || s_testShout.load(std::memory_order_relaxed));
    if (!held)
        m_ritualLatched = false;
    if (fallen != m_ritualTarget)
    {
        m_ritualTarget = fallen;
        m_ritualSince = 0;
    }
    if (!held || m_ritualLatched)
        m_ritualSince = 0;
    else if (!m_ritualSince)
    {
        m_ritualSince = aNow;
        PlayArt(aPlayer, kRitualArt, 2.2f);
        spdlog::info("Revive: calling back {} started", Name(fallen));
    }
    const double progress = m_ritualSince ?
        (std::min)(1.0, static_cast<double>(aNow - m_ritualSince) / static_cast<double>(kRitualMs)) : 0.0;
    if (m_ritualSince && aNow - m_ritualSince >= kRitualMs)
    {
        // All magicka is spent; the server moves the fallen player beside this one.
        s_ritualConsumed.store(true, std::memory_order_relaxed);
        m_ritualLatched = true;
        m_ritualSince = 0;
        aPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kMagicka, 0.f);
        m_magickaLockUntil = aNow + kMagickaLockMs;
        ReviveRequest request;
        request.Action = ReviveAction::Raise;
        request.Epoch = m_epoch;
        request.Revision = m_revision;
        request.PlayerId = fallen;
        m_transport.Send(request);
        spdlog::info("Revive: called back {} (magicka spent)", Name(fallen));
        Notice(fmt::format("You call {} back from the void", Name(fallen)));
    }
    const auto shout = ReadControl("Shout");
    const std::string note = combat ? "Cannot call back the fallen in combat" :
        m_magickaLockUntil ? std::string() : !full ? "Requires full magicka" : std::string();
    PushUi(5, Name(fallen), shout.Key.empty() ? shout.Label : shout.Key, progress, "Call back", aNow, note, shout.Button,
        shout.Gamepad);
    return true;
}
