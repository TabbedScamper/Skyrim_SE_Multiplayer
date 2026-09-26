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
#include <Messages/PlayerRespawnRequest.h>
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
    aActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth,
        (std::max)(1.f, maximum * aFraction));
    EndBleedout(aActor);
}

bool GroundedAndDry(PlayerCharacter* aPlayer) noexcept
{
    if (!aPlayer->currentProcess || !aPlayer->GetNiNode() || !aPlayer->parentCell)
        return false;
    using TController = uint8_t*(AIProcess*);
    using TState = uint32_t(const void*);
    using TWater = bool(TESObjectCELL*, const NiPoint3*, float*);
    POINTER_SKYRIMSE(TController, controller, 39856);
    POINTER_SKYRIMSE(TState, state, 62427);
    POINTER_SKYRIMSE(TWater, water, 19002);
    auto* object = controller.Get()(aPlayer->currentProcess);
    // Actor::IsInMidair reads this context; require OnGround, excluding jumping and swimming too.
    if (!object || state.Get()(object + 0x1E0) != 0)
        return false;
    float height{};
    water.Get()(aPlayer->parentCell, &aPlayer->position, &height);
    return std::isfinite(height) && height <= aPlayer->position.z;
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
};

ActivateInput ReadActivate() noexcept
{
    ActivateInput result;
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
            if (!entry.Event || std::strcmp(entry.Event, "Activate") != 0 || entry.Key == 0xFF)
                continue;
            if (device == 0)
            {
                const auto scan = entry.Key & 0x80 ? 0xE000 | (entry.Key & 0x7F) : entry.Key;
                const auto key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
                char label[64]{};
                GetKeyNameTextA((entry.Key & 0x7F) << 16 | (entry.Key & 0x80 ? 1 << 24 : 0), label, sizeof(label));
                if (*label)
                    result.Label = label;
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
                    const char* label = entry.Key == XINPUT_GAMEPAD_A ? "A" : entry.Key == XINPUT_GAMEPAD_B ? "B" :
                        entry.Key == XINPUT_GAMEPAD_X ? "X" : entry.Key == XINPUT_GAMEPAD_Y ? "Y" :
                        entry.Key == 9 ? "LT" : entry.Key == 10 ? "RT" : "Activate (controller)";
                    result.Label += std::string(" / ") + label;
                    break;
                }
            }
            break;
        }
    }
    return result;
}
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

void ReviveService::Reset() noexcept
{
    CancelHold();
    for (const auto& [id, peer] : m_peers)
        if (peer.AppliedDown)
            if (auto* actor = FindPlayer(id))
                actor->SetNoBleedoutRecovery(false);
    m_peers.clear();
    m_safePositions.clear();
    m_active = m_down = false;
    m_nextState = m_nextNotice = m_noHelpSince = m_safeSince = m_lastUpdate = 0;
    m_safeCell = 0;
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

void ReviveService::TrackSafePosition(PlayerCharacter* aPlayer, uint64_t aNow) noexcept
{
    const auto* cell = aPlayer->GetParentCellEx();
    const auto delta = aPlayer->position - m_lastPosition;
    const bool continuous = m_lastUpdate && aNow - m_lastUpdate < 500 &&
        glm::dot(delta, delta) < 1000.f * 1000.f;
    m_lastUpdate = aNow;
    m_lastPosition = aPlayer->position;
    if (!cell || m_down || aPlayer->IsInCombat() || !m_alive || !PlayerCollision::LocalHasFreeControl() || !GroundedAndDry(aPlayer) ||
        !std::isfinite(aPlayer->position.x) || !std::isfinite(aPlayer->position.y) || !std::isfinite(aPlayer->position.z))
    {
        m_safeSince = 0;
        return;
    }
    if (!continuous || m_safeCell != cell->formID || !m_safeSince)
    {
        m_safeCell = cell->formID;
        m_safeSince = aNow;
    }
    if (aNow - m_safeSince >= 3000)
        m_safePositions[cell->formID] = {cell->formID, cell->worldspace ? cell->worldspace->formID : 0,
            aPlayer->position, aNow};
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

void ReviveService::Fallback(PlayerCharacter* aPlayer, const char* aReason) noexcept
{
    auto* cell = aPlayer->GetParentCellEx();
    if (!cell)
        return;
    const SafePosition* safe = nullptr;
    if (const auto it = m_safePositions.find(cell->formID); it != m_safePositions.end())
        safe = &it->second;
    // Crossing an exterior cell boundary must not discard the last safe spot in this worldspace.
    if (!safe && cell->worldspace)
        for (const auto& [id, position] : m_safePositions)
            if (position.WorldSpace == cell->worldspace->formID && (!safe || safe->Recorded < position.Recorded))
                safe = &position;
    auto* destination = safe ? Cast<TESObjectCELL>(TESForm::GetById(safe->Cell)) : nullptr;
    aPlayer->PayCrimeGoldToAllFactions();
    if (destination)
    {
        aPlayer->RespawnPlayerAt(destination, safe->Position);
        spdlog::info("Revive: fallback respawn at last safe spot ({})", aReason);
    }
    else if (!cell->worldspace)
    {
        aPlayer->RespawnPlayer();
        Recover(aPlayer, 1.f);
        spdlog::info("Revive: fallback respawn at interior start ({}, no safe spot recorded)", aReason);
    }
    else
    {
        // No arbitrary COC placement when a party formed in combat before any safe sample existed.
        aPlayer->RespawnPlayerAt(cell, aPlayer->position);
        spdlog::info("Revive: fallback respawn in place ({}, no safe exterior spot recorded)", aReason);
    }
    RestoreSpells(aPlayer);
    m_transport.Send(PlayerRespawnRequest());
    m_down = false;
    ++m_revision;
    m_noHelpSince = m_safeSince = 0;
    SendState(aPlayer, GetTickCount64());
}

void ReviveService::OnNotify(const NotifyRevive& aMessage) noexcept
{
    if (!aMessage.IsValid() || !m_active || aMessage.Epoch != m_epoch || !IsPartyMember(aMessage.PlayerId))
        return;
    if (aMessage.Action == ReviveAction::Grant)
    {
        auto* player = PlayerCharacter::Get();
        if (aMessage.PlayerId != m_transport.GetLocalPlayerId() || !m_down || !player ||
            aMessage.Revision != m_revision || !IsPartyMember(aMessage.ReviverId))
            return;
        const auto reviver = m_peers.find(aMessage.ReviverId);
        if (reviver == m_peers.end() || GetTickCount64() - reviver->second.Received > 3500 ||
            reviver->second.Data.Down || !reviver->second.Data.Alive || reviver->second.Data.InCombat ||
            !Near(player, reviver->second, 200.f))
            return;
        Recover(player, 0.5f);
        m_down = false;
        ++m_revision;
        m_noHelpSince = m_nextNotice = 0;
        RestoreSpells(player);
        spdlog::info("Revive: revived by {}", Name(aMessage.ReviverId));
        Notice(fmt::format("Revived by {}", Name(aMessage.ReviverId)));
        SendState(player, GetTickCount64());
        return;
    }
    if (aMessage.PlayerId == m_transport.GetLocalPlayerId())
        return;
    auto& peer = m_peers[aMessage.PlayerId];
    if (aMessage.Revision < peer.Data.Revision)
        return;
    if (aMessage.Down && (!peer.Received || !peer.Data.Down || aMessage.Revision != peer.Data.Revision))
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
        if (peer.AppliedForm == actor->formID && peer.AppliedRevision == peer.Data.Revision)
            continue;
        if (peer.Data.Down)
        {
            actor->SetNoBleedoutRecovery(true);
            LifeState(actor, 7);
            BSFixedString start("BleedoutStart");
            actor->SendAnimationEvent(&start);
        }
        else if (peer.AppliedDown || actor->actorState.IsBleedingOut())
        {
            // Health is replicated by the owner's actor-value stream, including full-health fallbacks.
            actor->SetNoBleedoutRecovery(false);
            EndBleedout(actor);
        }
        peer.AppliedForm = actor->formID;
        peer.AppliedRevision = peer.Data.Revision;
        peer.AppliedDown = peer.Data.Down;
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
        Reset();
        m_epoch = party.GetStartEpoch();
        m_active = true;
        ++m_revision;
    }
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    if (!player || !ui || !player->parentCell || !player->GetNiNode() ||
        ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu")))
    {
        CancelHold();
        m_safeSince = m_noHelpSince = 0;
        return true;
    }
    const auto now = GetTickCount64();
    const bool down = player->actorState.IsBleedingOut();
    if (down != m_down)
    {
        m_down = down;
        ++m_revision;
        m_nextState = m_noHelpSince = m_safeSince = 0;
        CancelHold();
        if (down)
        {
            player->SetNoBleedoutRecovery(true);
            Notice("You are down. A nearby player can revive you. R: respawn at your last safe spot");
            spdlog::info("Revive: {} down", Name(m_transport.GetLocalPlayerId()));
            m_nextNotice = now + 6000;
            m_giveUpHeld = (GetAsyncKeyState('R') & 0x8000) != 0;
        }
    }
    const bool combat = player->IsInCombat();
    const bool alive = !player->actorState.IsDeadState() && player->GetActorValue(ActorValueInfo::kHealth) > 0.f;
    if (now >= m_nextState || combat != m_combat || alive != m_alive)
        SendState(player, now);
    std::erase_if(m_peers, [&](const auto& entry) {
        const bool expired = !IsPartyMember(entry.first) || now - entry.second.Received > 3500;
        if (expired && entry.second.AppliedDown)
            if (auto* actor = FindPlayer(entry.first))
                actor->SetNoBleedoutRecovery(false);
        return expired;
    });
    ApplyPeers();
    const bool paused = ui->numPausesGame || ui->numItemMenus || ui->modal ||
        ui->GetMenuOpen(BSFixedString("Console")) || m_world.GetOverlayService().GetActive();
    auto* window = BSGraphics::GetMainWindow();
    const bool input = !paused && window && GetForegroundWindow() == window->hWnd &&
        !ControlBindings::IsCapturing() && (m_down || PlayerCollision::LocalHasFreeControl()) &&
        !ui->GetMenuOpen(BSFixedString("Dialogue Menu"));
    if (paused)
    {
        m_safeSince = m_noHelpSince = 0;
        CancelHold();
        return true;
    }
    TrackSafePosition(player, now);
    if (m_down)
    {
        bool help = false;
        for (const auto& [id, peer] : m_peers)
            if (peer.Data.Alive && !peer.Data.Down && Near(player, peer, 5000.f))
                help = true;
        if (help)
            m_noHelpSince = 0;
        else if (!m_noHelpSince)
            m_noHelpSince = now;
        const bool giveUp = input && (GetAsyncKeyState('R') & 0x8000) != 0;
        if (giveUp && !m_giveUpHeld)
            Fallback(player, "give up");
        else if (m_noHelpSince && now - m_noHelpSince >= 10000)
            Fallback(player, "no nearby living party player");
        m_giveUpHeld = giveUp;
        if (m_down && now >= m_nextNotice)
        {
            Notice("You are down. A nearby player can revive you. R: respawn at your last safe spot");
            m_nextNotice = now + 6000;
        }
        return true;
    }
    m_mainSpell = player->magicItems[0] ? player->magicItems[0]->formID : 0;
    m_secondarySpell = player->magicItems[1] ? player->magicItems[1]->formID : 0;
    m_power = player->equippedShout ? player->equippedShout->formID : 0;
    if (!input || !alive)
    {
        CancelHold();
        return true;
    }
    const auto activate = ReadActivate();
    uint32_t target{};
    uint64_t revision{};
    float closest = 200.f * 200.f;
    for (const auto& [id, peer] : m_peers)
    {
        if (!peer.Data.Down || !Near(player, peer, 250.f))
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
    if (!target || combat || !activate.Held)
    {
        CancelHold();
        if (target && now >= m_nextNotice)
        {
            Notice(combat ? "Clear the fight first" : fmt::format("Hold {} on {} to revive", activate.Label, Name(target)));
            m_nextNotice = now + 3000;
        }
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
    if (now >= m_nextHold)
    {
        SendHold(ReviveAction::Hold);
        if (now - m_holdSince >= 3000)
            SendHold(ReviveAction::Finish);
        m_nextHold = now + 250;
    }
    if (now >= m_nextNotice)
    {
        Notice(fmt::format("Reviving {}... {}/3", Name(target), (std::min)(uint64_t{3}, (now - m_holdSince) / 1000)));
        m_nextNotice = now + 1000;
    }
    return true;
}
