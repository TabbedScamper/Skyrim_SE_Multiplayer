#include <Services/CutsceneFollow.h>
#include <Services/Generic/UnstuckReset.h>
#include <Services/CreatorTogether.h>
#include <Messages/LeaderControlRequest.h>
#include <Messages/NotifyLeaderControl.h>
#include <Messages/RequestPlayerControlState.h>
#include <Messages/NotifyPlayerControlState.h>
#include <Services/PlayerCollision.h>
#include <Services/PartyService.h>
#include <Services/LobbyService.h>
#include <Services/ReviveService.h>
#include <Games/References.h>
#include <Services/CharacterService.h>
#include <Services/CharacterSnapshots.h>
#include <Services/DoorVoteService.h>
#include <Services/DropInService.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#include <Services/TransportService.h>
#include <Services/SteamLobbyService.h>
#include <Services/OverlayService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyChangeLeaderRequest.h>
#include <Messages/PartyKickRequest.h>
#include <Messages/PartyReadyRequest.h>
#include <Messages/PartyStartRequest.h>
#include <Messages/PartySessionSettingsRequest.h>
#include <Messages/PartyGameplaySettingsRequest.h>
#include <Messages/CheckpointSaveRequest.h>
#include <Messages/NotifyCheckpointSave.h>
#include <Messages/CheckpointRoster.h>
#include <Messages/HostHandoff.h>
#include <HandoffPolicy.h>
#include <Services/CheckpointSaves.h>
#include <Utils/MainThread.h>
#include <Forms/TESIdleForm.h>

#include <OverlayApp.hpp>

#include <Forms/TESGlobal.h>
#include <Games/TES.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Forms/TESNPC.h>
#include <AI/AIProcess.h>

namespace
{
// Research: CommonLibSSE-NG src/RE/C/ControlMap.cpp and Actor's GetCurrentScene
// contract (https://github.com/alandtse/CommonLibSSE-NG). Use native accessors;
// older CommonLib layouts place the masks eight bytes too early on 1.7.104.
void ReadControlState(BSInputEnableManager* apMap, uint32_t& aLive, uint32_t& aStored)
{
    // ControlMap::GetControlsState, ID 68548, VA 140CEFB80.
    using TGet = void(BSInputEnableManager*, uint32_t&, uint32_t&);
    POINTER_SKYRIMSE(TGet, get, 68548);
    get.Get()(apMap, aLive, aStored);
}

bool PlayerSequenceEnded(PlayerCharacter* apPlayer)
{
    if (!apPlayer || !apPlayer->parentCell || !apPlayer->GetNiNode())
        return false;
    const auto flags = apPlayer->actorState.flags1;
    const auto life = (flags >> 21) & 0xF;
    // A leftover restraint (life state 6) is repairable only after the native scene,
    // package, furniture transition and AI ownership have all finished.
    if ((life != 0 && life != 6) || ((flags >> 14) & 0xF) || ((flags >> 25) & 7) ||
        (apPlayer->currentProcess && apPlayer->currentProcess->package))
        return false;
    // PlayerCharacter::SetAIDriven, ID 40586, VA 1407559F0.
    if ((*(reinterpret_cast<const uint8_t*>(apPlayer) + 0xBEA) & 8) != 0)
        return false;
    // Actor::GetCurrentScene, slot 0x4A, ID 37247, VA 140674B70.
    // This tests this actor's scene, not background scenes waiting for player movement.
    using TScene = BGSScene*(Actor*);
    POINTER_SKYRIMSE(TScene, getScene, 37247);
    const auto* pScene = getScene.Get()(apPlayer);
    return !pScene || !pScene->isPlaying;
}

bool CapturePlayerControlState(PlayerControlState& aState)
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pControls = PlayerControls::GetInstance();
    auto* pMap = BSInputEnableManager::Get();
    if (!pPlayer || !pMap || !pControls || !pControls->pMovementHandler ||
        !pControls->pLookHandler || !pControls->togglePOVHandler)
        return false;
    uint32_t live{}, stored{};
    ReadControlState(pMap, live, stored);
    aState.Controls = live & PlayerControlState::kChannels;
    aState.Handlers = (pControls->pMovementHandler->isEnabled ? 1 : 0) |
        (pControls->pLookHandler->isEnabled ? 2 : 0) | (pControls->togglePOVHandler->isEnabled ? 4 : 0);
    aState.PovScript = pControls->Data.povScriptMode;
    aState.Restrained = ((pPlayer->actorState.flags1 >> 21) & 0xF) == 6;
    aState.Free = !aState.Restrained && !pControls->bBlockPlayerInput &&
        (aState.Controls & 3) == 3 && (aState.Handlers & 3) == 3 &&
        stored == 0x80000000u && PlayerSequenceEnded(pPlayer);
    return true;
}

struct PlayerControlSync
{
    // F8 is explicit host authority. Match both locked and unlocked policies now,
    // even while a local scene owns saved controls. Do not import the host's UI
    // contexts or change the local dead/bleedout life states.
    bool ForceApply(const PartyUnstuckState& acState, uint32_t& aBefore, uint32_t& aAfter)
    {
        auto* player = PlayerCharacter::Get();
        auto* controls = PlayerControls::GetInstance();
        auto* map = BSInputEnableManager::Get();
        if (!player || !map || !controls || !controls->pMovementHandler ||
            !controls->pLookHandler || !controls->togglePOVHandler)
            return false;
        const auto life = (player->actorState.flags1 >> 21) & 0xF;
        if (life != 0 && life != 6)
            return false;
        uint32_t live{}, stored{};
        ReadControlState(map, live, stored);
        aBefore = live & PlayerControlState::kChannels;
        using TRestrain = void(Actor*, bool);
        POINTER_SKYRIMSE(TRestrain, restrain, 37488);
        if ((life == 6) != acState.Controls.Restrained)
            restrain.Get()(player, acState.Controls.Restrained);
        // 68545 / 140CEFAA0 updates saved masks only when one exists. Apply all
        // channels, not just the live delta, so furniture exit cannot restore a
        // stale saved lock when live controls already happen to match the host.
        map->EnableOtherEvent(PlayerControlState::kChannels & ~acState.Controls.Controls, false, true);
        map->EnableOtherEvent(acState.Controls.Controls, true, true);
        controls->pMovementHandler->isEnabled = (acState.Controls.Handlers & 1) != 0;
        controls->pLookHandler->isEnabled = (acState.Controls.Handlers & 2) != 0;
        controls->togglePOVHandler->isEnabled = (acState.Controls.Handlers & 4) != 0;
        controls->Data.povScriptMode = acState.Controls.PovScript;
        controls->SetBlockPlayerInput(acState.InputBlocked);
        ReadControlState(map, live, stored);
        aAfter = live & PlayerControlState::kChannels;
        Ready = false;
        SettledSince = 0;
        // The reset sequence has a separate ordering domain. Never insert it
        // into State; the next ordinary heartbeat resumes the normal policy.
        NextApplyMs = GetTickCount64() + 1000;
        return aAfter == acState.Controls.Controls;
    }

    PlayerControlSync(PartyService& aParty, World& aWorld, entt::dispatcher& aDispatcher)
        : Party(aParty), GameWorld(aWorld)
        , Connection(aDispatcher.sink<NotifyPlayerControlState>().connect<&PlayerControlSync::OnState>(this))
        , Disconnect(aDispatcher.sink<DisconnectedEvent>().connect<&PlayerControlSync::OnDisconnect>(this))
    {
    }

    void OnDisconnect(const DisconnectedEvent&) { Reset(); }

    void Reset()
    {
        State = {};
        Sent = {};
        ReceivedMs = NextSendMs = NextApplyMs = SettledSince = 0;
        Leader = 0;
        Ready = false;
    }

    void OnState(const NotifyPlayerControlState& acMessage)
    {
        const auto& state = acMessage.State;
        if (!Party.IsInParty() || Party.IsLeader() || Party.GetSessionState() < 2 ||
            acMessage.LeaderId != Party.GetLeaderPlayerId() || state.Epoch != Party.GetStartEpoch() ||
            !state.IsValid())
            return;
        if (Leader == acMessage.LeaderId && State.Epoch == state.Epoch && State.Sequence >= state.Sequence)
            return;
        if (Leader != acMessage.LeaderId || State.Epoch != state.Epoch || !state.Free)
            SettledSince = 0;
        State = state;
        Leader = acMessage.LeaderId;
        ReceivedMs = GetTickCount64();
        Ready = false;
    }

    bool HasRelease() const
    {
        return Party.IsInParty() && !Party.IsLeader() && Party.GetSessionState() >= 3 &&
            State.Free && State.Epoch == Party.GetStartEpoch() && Leader == Party.GetLeaderPlayerId() &&
            GetTickCount64() - ReceivedMs < 3500;
    }

    void Update()
    {
        if (!Party.IsInParty() || !Party.GetStartEpoch() || Party.GetSessionState() < 2)
        {
            Reset();
            return;
        }
        const auto now = GetTickCount64();
        if (Party.IsLeader())
        {
            RequestPlayerControlState request;
            auto& state = request.State;
            state.Epoch = Party.GetStartEpoch();
            if (!CapturePlayerControlState(state))
                return;
            state.Free = state.Free && Party.GetSessionState() >= 3;
            // Compare policy without the wire sequence; send every change and a 1 s heartbeat.
            if (state == Sent && now < NextSendMs)
                return;
            Sent = state;
            state.Sequence = ++Sequence;
            NextSendMs = now + 1000;
            GameWorld.GetTransport().Send(request);
            return;
        }
        auto* pPlayer = PlayerCharacter::Get();
        auto* pControls = PlayerControls::GetInstance();
        const auto* pUI = UI::Get();
        if (!HasRelease() || CutsceneFollow::IsActive() || !PlayerSequenceEnded(pPlayer) ||
            !pControls || pControls->bBlockPlayerInput || !pUI || pUI->numPausesGame ||
            pUI->numItemMenus || pUI->modal || pUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
            pUI->GetMenuOpen(BSFixedString("RaceSex Menu")) || pUI->GetMenuOpen(BSFixedString("Main Menu")) ||
            pUI->GetMenuOpen(BSFixedString("Dialogue Menu")) || GameWorld.GetOverlayService().GetActive())
        {
            Ready = false;
            SettledSince = 0;
            return;
        }
        if (!SettledSince)
            SettledSince = now;
        if (now - SettledSince < 500 || now < NextApplyMs)
            return;
        NextApplyMs = now + 250;
        Ready = false;
        auto* pMap = BSInputEnableManager::Get();
        if (!pMap || !pControls->pMovementHandler || !pControls->pLookHandler || !pControls->togglePOVHandler)
            return;
        uint32_t live{}, stored{};
        ReadControlState(pMap, live, stored);
        // A local engine state owns a saved mask; do not overwrite its recovery. Known gap (2026-10-01): after loading a
        // save made mid-execution, the follower keeps a saved mask from its own cart scene that only the host's scene
        // clears, and stays locked. A blanket "take the host's mask after 3 s" fix was refuted in review (it would override
        // mounts and legitimate script locks, persistently); a fix must be load-scoped. See FINDINGS 2026-10-01.
        if (stored != 0x80000000u)
            return;
        const auto before = live & PlayerControlState::kChannels;
        const uint32_t enable = State.Controls & ~before;
        const uint32_t disable = before & ~State.Controls;
        // ID 68545, VA 140CEFAA0: use ToggleControls so handlers receive UserEventEnabled.
        if (disable)
            pMap->EnableOtherEvent(disable, false, false);
        if (enable)
            pMap->EnableOtherEvent(enable, true, false);
        const auto handlers = (pControls->pMovementHandler->isEnabled ? 1 : 0) |
            (pControls->pLookHandler->isEnabled ? 2 : 0) | (pControls->togglePOVHandler->isEnabled ? 4 : 0);
        const bool restrained = ((pPlayer->actorState.flags1 >> 21) & 0xF) == 6;
        if (restrained && !State.Restrained)
        {
            // Actor::SetRestrained(false), ID 37488, VA 140687B40. Goes through
            // SetLifeState, including its native exit side effects; never write life bits.
            using TRestrain = void(Actor*, bool);
            POINTER_SKYRIMSE(TRestrain, restrain, 37488);
            restrain.Get()(pPlayer, false);
        }
        if (enable || disable || handlers != State.Handlers || restrained ||
            pControls->Data.povScriptMode != State.PovScript)
            spdlog::info("Follower control release: epoch={} sequence={} controls={:03X}->{:03X} handlers={}->{} restrained={}->{}",
                State.Epoch, State.Sequence, before, State.Controls, handlers, State.Handlers, restrained, State.Restrained);
        pControls->pMovementHandler->isEnabled = (State.Handlers & 1) != 0;
        pControls->pLookHandler->isEnabled = (State.Handlers & 2) != 0;
        pControls->togglePOVHandler->isEnabled = (State.Handlers & 4) != 0;
        pControls->Data.povScriptMode = State.PovScript;
        Ready = true;
    }

    PartyService& Party;
    World& GameWorld;
    PlayerControlState State{}, Sent{};
    uint64_t Sequence{}, ReceivedMs{}, NextSendMs{}, NextApplyMs{}, SettledSince{};
    uint32_t Leader{};
    bool Ready{};
    entt::scoped_connection Connection, Disconnect;
};

std::unique_ptr<PlayerControlSync> s_playerControlSync;
}

bool UnstuckControls::Capture(PlayerControlState& aState) noexcept
{
    return CapturePlayerControlState(aState);
}

bool UnstuckControls::Apply(const PartyUnstuckState& acState, uint32_t& aBefore, uint32_t& aAfter) noexcept
{
    return s_playerControlSync && s_playerControlSync->ForceApply(acState, aBefore, aAfter);
}

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransportService) noexcept
    : m_world(aWorld)
    , m_transport(aTransportService)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this);
    m_leaderControlConnection = aDispatcher.sink<NotifyLeaderControl>().connect<&PartyService::OnNotifyLeaderControl>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&PartyService::OnDisconnected>(this);

    m_playerListConnection = aDispatcher.sink<NotifyPlayerList>().connect<&PartyService::OnPlayerList>(this);
    m_partyInfoConnection = aDispatcher.sink<NotifyPartyInfo>().connect<&PartyService::OnPartyInfo>(this);
    m_checkpointSaveConnection = aDispatcher.sink<NotifyCheckpointSave>().connect<&PartyService::OnCheckpointSave>(this);
    m_checkpointRosterConnection = aDispatcher.sink<NotifyCheckpointRoster>().connect<&PartyService::OnCheckpointRoster>(this);
    m_partyInviteConnection = aDispatcher.sink<NotifyPartyInvite>().connect<&PartyService::OnPartyInvite>(this);
    m_partyJoinedConnection = aDispatcher.sink<NotifyPartyJoined>().connect<&PartyService::OnPartyJoined>(this);
    m_partyLeftConnection = aDispatcher.sink<NotifyPartyLeft>().connect<&PartyService::OnPartyLeft>(this);
    s_playerControlSync = std::make_unique<PlayerControlSync>(*this, aWorld, aDispatcher);
}

void PartyService::CreateParty() const noexcept
{
    PartyCreateRequest request;
    m_transport.Send(request);
}

void PartyService::LeaveParty() noexcept
{
    if (m_isLeader && m_sessionState >= 2 && m_partyMembers.size() > 1)
    {
        BeginHostDeparture(false);
        return;
    }
    m_world.GetSteamLobbyService().ForgetParty();
    const bool wasInGame = m_inParty && m_sessionState >= 2;
    if (wasInGame)
        CheckpointSaves::WriteCarryHome();
    PartyLeaveRequest request;
    m_transport.Send(request);
    if (wasInGame)
        LobbyService::RequestReturnToLobby("party_left");
}

void PartyService::CreateInvite(const uint32_t aPlayerId) const noexcept
{
    PartyInviteRequest request;
    request.PlayerId = aPlayerId;
    m_transport.Send(request);
}

void PartyService::AcceptInvite(const uint32_t aInviterId) const noexcept
{
    if (!m_invitations.contains(aInviterId))
        return;

    PartyAcceptInviteRequest request;
    request.InviterId = aInviterId;
    m_transport.Send(request);
}

void PartyService::KickPartyMember(const uint32_t aPlayerId) const noexcept
{
    PartyKickRequest kickMessage;
    kickMessage.PartyMemberPlayerId = aPlayerId;
    m_transport.Send(kickMessage);
}

void PartyService::EndParty() noexcept
{
    if (!m_inParty || !m_isLeader || m_partyMembers.size() < 2)
        return;
    if (m_sessionState >= 2)
    {
        BeginHostDeparture(false);
        return;
    }
    m_world.GetSteamLobbyService().ForgetParty();
    // The server sends a forced leave to every member before it removes the host.
    PartyKickRequest endMessage;
    endMessage.PartyMemberPlayerId = 0;
    m_transport.Send(endMessage);
}

void PartyService::DrainForcedLeave() noexcept
{
    const int pending = m_forcedLeavePending.exchange(0);
    if (!pending)
        return;
    // The server's notification can arrive on a VM worker. Capture on the main loop before Steam/transport cleanup.
    if (pending == 2)
        CheckpointSaves::WriteCarryHome();
    m_world.GetSteamLobbyService().QueueLeaveSession(true);
}

void PartyService::ChangePartyLeader(const uint32_t aPlayerId) const noexcept
{
    PartyChangeLeaderRequest changeMessage;
    changeMessage.PartyMemberPlayerId = aPlayerId;
    m_transport.Send(changeMessage);
}

bool PartyService::BeginHostDeparture(bool aQuitToMenu) noexcept
{
    if (!m_inParty || !m_isLeader || m_sessionState < 2 || m_partyMembers.size() < 2)
        return false;
    if (m_handoffDeparted)
        return false;
    m_handoffQuitRequested |= aQuitToMenu;
    if (m_handoffTargetSteamId)
        return true;
    for (const auto id : m_partyMembers)
        if (id != m_leaderPlayerId)
        {
            ChangePartyLeader(id);
            spdlog::info("Handoff: leader departure requested successor player {}", id);
            return true;
        }
    return false;
}

std::string PartyService::HandoffStatus() const noexcept
{
    return fmt::format(R"({{"targetSteamId":"{}","checkpoint":"{}","ready":{},"ackSent":{},"departed":{},"seeded":{},"members":{},"lastFailure":"{}"}})",
        m_handoffTargetSteamId, m_handoffCheckpointId.c_str(), m_handoffReady, m_handoffAckSent,
        m_handoffDeparted, m_handoffCampaign, m_handoffOrder.size(), m_lastHandoffFailure.c_str());
}

void PartyService::CancelHandoff() noexcept
{
    m_lastHandoffFailure = m_handoffSeedCheckpoint;
    m_handoffDeparted = false;
    m_handoffCampaign = false;
    m_handoffOrder.clear();
    m_handoffTargetSteamId = 0;
    m_handoffCheckpointId.clear();
    m_handoffSeedCheckpoint.clear();
    m_handoffAvailabilitySent = false;
    m_handoffSaveRequested = false;
    m_handoffAckSent = false;
    m_handoffReady = false;
}

bool PartyService::IsFollowerCinematicInputGated() const noexcept
{
    // Camera authority is a party-phase policy, independent of the local
    // input bit: native character creation must temporarily accept input.
    return m_inParty && !m_isLeader && m_sessionState == 2 && m_startEpoch != 0;
}

void PartyService::SetReady(const bool aReady) const noexcept
{
    PartyReadyRequest request;
    request.Ready = aReady;
    m_transport.Send(request);
}

void PartyService::SelectCampaign(const uint8_t aMode, const String& acCheckpointId) const noexcept
{
    PartyStartRequest request;
    request.Mode = aMode;
    // Continue loads the leader's newest matched checkpoint on every PC unless one was named.
    request.CheckpointId = aMode == PartyStartRequest::kContinue && acCheckpointId.empty() ?
        (m_handoffCampaign ? m_currentCheckpoint : CheckpointSaves::Latest()) : acCheckpointId;
    m_transport.Send(request);
}

void PartyService::StartTogether(const uint8_t aMode, const String& acCheckpointId) const noexcept
{
    PartyStartRequest request;
    request.Mode = aMode;
    request.Launch = true;
    request.CheckpointId = aMode == PartyStartRequest::kContinue && acCheckpointId.empty() ?
        (m_handoffCampaign ? m_currentCheckpoint : CheckpointSaves::Latest()) : acCheckpointId;
    m_transport.Send(request);
}

void PartyService::SetSessionSettings(const bool aOpen, const String& acPassword, bool aKeepPassword) const noexcept
{
    if (!m_isLeader)
        return;
    PartySessionSettingsRequest request;
    request.Open = aOpen;
    request.Password = acPassword;
    request.KeepPassword = aKeepPassword;
    m_transport.Send(request);
}

void PartyService::SetGameplaySettings(const uint32_t aDifficulty, const bool aPvpEnabled) const noexcept
{
    if (!m_isLeader || aDifficulty > 5)
        return;
    PartyGameplaySettingsRequest request{};
    request.Difficulty = aDifficulty;
    request.PvpEnabled = aPvpEnabled;
    m_transport.Send(request);
}

void PartyService::NoteGameLoaded() noexcept
{
    m_reloadPending = false;
    // A load clears the fade; stay black until everyone is in.
    if (m_wipeFade)
        FadeOutGame(true, true, 0.f, true, 0.f);
}

void PartyService::ReachWorldReadyBarrier() noexcept
{
    // During an in-game reload the world is the old one until the load event (the in-world fallback below reported
    // the barrier the instant the reload began, and a waiting follower never reloaded).
    if (!m_waitingForWorldReady || m_sessionState != 1 || m_worldGateHeld || m_reloadPending)
        return;

    // Owner (2026-09-28): no frozen world while the others load; this PC waits on black and everyone fades in together
    // at the gameplay barrier.
    FadeOutGame(true, true, 0.f, true, 0.f);
    if (!m_wipeFade)
        m_wipeFadeSince = GetTickCount64();
    m_wipeFade = true;

    if (auto* pUI = UI::Get())
    {
        ++pUI->numPausesGame;
        m_worldGateHeld = true;
    }
    if (auto* pControls = PlayerControls::GetInstance())
        pControls->SetBlockPlayerInput(true);

    PartyReadyRequest request;
    request.Ready = true;
    m_transport.Send(request);
    spdlog::info("Reached shared-campaign world-ready barrier for epoch {}", m_startEpoch);
}

void PartyService::OnCheckpointSave(const NotifyCheckpointSave& acMessage) noexcept
{
    spdlog::info("Checkpoint {} announced (epoch {}, ours {}, session {})", acMessage.CheckpointId, acMessage.AuthorityEpoch,
        m_startEpoch, m_sessionState);
    if (!m_inParty || acMessage.AuthorityEpoch != m_startEpoch)
        return;
    m_currentCheckpoint = acMessage.CheckpointId;
    m_pendingCheckpoint = acMessage.CheckpointId;
    if (m_isLeader)
    {
        if (m_saveRoster && m_saveRosterId == acMessage.CheckpointId)
            SendRosterPosition(acMessage.CheckpointId, *m_saveRoster);
        else if (auto position = CaptureRosterPosition())
            SendRosterPosition(acMessage.CheckpointId, *position);
        m_saveRoster.reset();
    }
}

std::optional<RosterPosition> PartyService::CaptureRosterPosition() noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* cell = player ? player->parentCell : nullptr;
    auto* npc = player ? Cast<TESNPC>(player->baseForm) : nullptr;
    if (!cell || !npc) return std::nullopt;
    RosterPosition position;
    position.SteamId = m_world.GetSteamLobbyService().LocalSteamId();
    position.CharacterName = npc->fullName.value.AsAscii();
    const auto key = CheckpointSaves::CurrentCharacterKey();
    position.CharacterKey = (key.empty() ? "name:" + std::string(position.CharacterName.c_str()) : key).c_str();
    if (cell->worldspace && !m_world.GetModSystem().GetServerModId(cell->worldspace->formID, position.WorldSpaceId))
        return std::nullopt;
    if (!m_world.GetModSystem().GetServerModId(cell->formID, position.CellId))
        return std::nullopt;
    position.X = player->position.x;
    position.Y = player->position.y;
    position.Z = player->position.z;
    position.Heading = player->rotation.z;
    position.Downed = player->actorState.IsDeadState() || player->actorState.IsBleedingOut();
    if (!position.SteamId || !position.Valid())
    {
        spdlog::warn("Checkpoint roster skipped: Steam identity or local position is unavailable");
        return std::nullopt;
    }
    return position;
}

void PartyService::SendRosterPosition(const String& acId, const RosterPosition& acPosition) noexcept
{
    m_world.GetDropInService().SetCheckpointPosition(acPosition);
    RequestCheckpointRoster request;
    request.CheckpointId = acId;
    request.Position = acPosition;
    m_transport.Send(request);
}

void PartyService::SendHandoffSeed(const String& acCheckpointId) noexcept
{
    if (acCheckpointId.empty())
    {
        spdlog::error("Handoff: no checkpoint from the lost party to establish campaign lineage");
        return;
    }
    String checkpoint = acCheckpointId;
    if (checkpoint.empty() || !CheckpointSaves::Has(checkpoint) || !m_checkpointRosters.contains(checkpoint))
    {
        for (const auto& candidate : CheckpointSaves::Available(32))
            if (m_checkpointRosters.contains(candidate) &&
                (acCheckpointId.empty() || HandoffPolicy::SameFamily(candidate.c_str(), acCheckpointId.c_str())))
            { checkpoint = candidate; break; }
    }
    const auto it = m_checkpointRosters.find(checkpoint);
    if (it == m_checkpointRosters.end() || !CheckpointSaves::Has(checkpoint))
    {
        spdlog::error("Handoff: no complete local checkpoint roster to seed the new host");
        return;
    }
    RequestHandoffSeed seed;
    seed.CheckpointId = checkpoint;
    seed.SteamIds = m_handoffOrder;
    for (const auto steamId : seed.SteamIds)
    {
        const auto found = std::find_if(it->second.begin(), it->second.end(), [steamId](const RosterPosition& entry)
            { return entry.SteamId == steamId; });
        if (found == it->second.end())
        {
            spdlog::error("Handoff: checkpoint {} has no roster position for Steam {}", checkpoint, steamId);
            return;
        }
        seed.Entries.push_back(*found);
    }
    m_transport.Send(seed);
    m_handoffSeedCheckpoint = checkpoint;
    spdlog::info("Handoff: seeded checkpoint {} for {} ordered members", checkpoint, seed.SteamIds.size());
}

void PartyService::OnCheckpointRoster(const NotifyCheckpointRoster& acMessage) noexcept
{
    if (!m_inParty || acMessage.Entries.size() != m_partyMembers.size())
        return;
    if (CheckpointSaves::WriteRoster(acMessage.CheckpointId, acMessage.Entries))
    {
        if (!m_checkpointRosters.contains(acMessage.CheckpointId))
            m_rosterOrder.push_back(acMessage.CheckpointId);
        m_checkpointRosters[acMessage.CheckpointId] = acMessage.Entries;
        if (m_rosterOrder.size() > 32)
        {
            m_checkpointRosters.erase(m_rosterOrder.front());
            m_rosterOrder.erase(m_rosterOrder.begin());
        }
        spdlog::info("Checkpoint {} roster sidecar written ({} players)", acMessage.CheckpointId, acMessage.Entries.size());
    }
    else
        spdlog::error("Checkpoint {} roster sidecar could not be written", acMessage.CheckpointId);
}

namespace
{
// Survival Mode Creation (ccqdrsse001-survivalmode.esl). Survival_MainScript's OnUpdate (every 5 s) shows the
// Survival_StartPrompt message box on each PC once the player is outdoors with combat controls
// (Survival_MainScript.ModeCanBeEnabled) unless Survival_PlayerHasBeenPrompted (local 0x0E8DF) is 1; the answer sets
// Survival_ModeToggle and the same loop starts/stops the mode. Each PC answered its own prompt at Helgen's exit.
// Until the mode is party-wide, a co-op session marks the prompt as shown; the Gameplay Settings toggle still works.
void SuppressSurvivalPrompt() noexcept
{
    static uint32_t s_formId = UINT32_MAX;
    if (s_formId == UINT32_MAX)
    {
        auto* pMod = ModManager::Get()->GetByName("ccqdrsse001-survivalmode.esl");
        s_formId = pMod ? pMod->GetFormId(0x0E8DF) : 0;
    }
    auto* pPrompted = s_formId ? Cast<TESGlobal>(TESForm::GetById(s_formId)) : nullptr;
    if (!pPrompted || pPrompted->f == 1.f)
        return;
    pPrompted->f = 1.f;
    spdlog::info("Survival Mode: start prompt marked as shown for this co-op session");
}
} // namespace

void PartyService::LaunchCheckpoint(const String& acCheckpointId) noexcept
{
    if (!MT::CutEnabled(6))
    {
        LaunchCheckpointNow(acCheckpointId, m_campaignMode);
        return;
    }
    std::lock_guard lock(m_launchLock);
    m_queuedLaunchCheckpoint = acCheckpointId;
    m_queuedLaunchMode = m_campaignMode;
    m_queuedLaunchConnection = MT::ConnectionSerial.load(std::memory_order_acquire);
    m_queuedLaunch = true;
}

bool PartyService::HasPendingCheckpointLaunch() noexcept
{
    std::lock_guard lock(m_launchLock);
    return m_queuedLaunch;
}

bool PartyService::DrainPendingCheckpointLaunch() noexcept
{
    String checkpoint;
    uint8_t mode{};
    uint32_t connection{};
    {
        std::lock_guard lock(m_launchLock);
        if (!m_queuedLaunch)
            return false;
        checkpoint = std::move(m_queuedLaunchCheckpoint);
        mode = m_queuedLaunchMode;
        connection = m_queuedLaunchConnection;
        m_queuedLaunch = false;
    }
    if (connection != MT::ConnectionSerial.load(std::memory_order_acquire))
    {
        spdlog::warn("mtd_terminal_drop checkpoint launch from an old connection");
        return false;
    }
    LaunchCheckpointNow(checkpoint, mode);
    return true;
}

void PartyService::DrainRestartFadeOnMainFrame() noexcept
{
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S3_3)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    if (m_restartFadePending.exchange(false, std::memory_order_acq_rel))
        FadeOutGame(true, true, 0.5f, true, 0.f);
}

void PartyService::LaunchCheckpointNow(const String& acCheckpointId, uint8_t aMode) noexcept
{
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S7)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    if (m_handoffCampaign && (acCheckpointId.empty() || !CheckpointSaves::Has(acCheckpointId)))
    {
        spdlog::error("Handoff: Continue refused because common checkpoint '{}' is absent locally", acCheckpointId);
        return;
    }
    // Continue with a named checkpoint loads that save on every PC; without one (or if
    // this PC does not have it) fall back to the game's own Continue.
    if (aMode != PartyStartRequest::kContinue || acCheckpointId.empty() || !CheckpointSaves::Load(acCheckpointId))
    {
        if (aMode == PartyStartRequest::kContinue)
            spdlog::warn("Continue without a matched checkpoint ('{}'): loading this PC's last save", acCheckpointId);
        LaunchSharedCampaignFromMainMenu(aMode);
    }
}

void PartyService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    // The black screen of a load never outlives the reason for it: lifted when the party or connection is gone, or
    // when the gameplay barrier has not released everyone within 3 minutes (a stuck peer must not leave this player
    // blind; review 2026-09-29).
    if (m_wipeFade && (!m_transport.IsConnected() || !m_inParty || m_sessionState == 0 ||
        GetTickCount64() - m_wipeFadeSince > 180000))
    {
        spdlog::warn("Load fade lifted without the gameplay barrier (connected {}, in party {}, state {}, {} ms)",
            m_transport.IsConnected(), m_inParty, m_sessionState, GetTickCount64() - m_wipeFadeSince);
        FadeOutGame(false, true, 1.f, true, 0.f);
        m_wipeFade = false;
        // The world-ready gate went up with the fade: never leave the player paused and input-blocked behind it.
        if (m_worldGateHeld)
        {
            if (auto* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
                --pUI->numPausesGame;
            PlayerControls::QueueInputRelease();
            m_worldGateHeld = false;
            if (auto* pControls = PlayerControls::GetInstance())
                pControls->SetBlockPlayerInput(false);
        }
    }
    // A follower waiting for the leader's load: never forever (a leader that cannot load must not strand the party).
    if (m_deferredLaunch && (m_sessionState != 1 || !m_inParty))
        m_deferredLaunch = false;
    if (m_reloadPending && (m_sessionState != 1 || !m_inParty))
        m_reloadPending = false;
    else if (m_deferredLaunch && GetTickCount64() - m_deferredSince > 120000)
    {
        spdlog::warn("Continue: the leader did not load within 120 s, loading anyway");
        m_deferredLaunch = false;
        LaunchCheckpoint(m_deferredCheckpoint);
    }
    {
        static uint64_t s_nextSurvivalCheck{};
        if (const auto nowMs = GetTickCount64(); nowMs >= s_nextSurvivalCheck)
        {
            s_nextSurvivalCheck = nowMs + 2000;
            if (m_transport.IsConnected() && IsInParty() && GetSessionState() >= 2)
                SuppressSurvivalPrompt();
        }
    }

    // Followers are passive spectators while the leader drives MQ101's cart
    // sequence.  Their local Havok world can briefly disagree with the host
    // while cells and the cart settle, so do not allow that private simulation
    // to kill the network player before the character-creation handoff.
    RefreshFollowerIntroProtection();

    // The intro camera lock must not swallow input in Skyrim's own character
    // creator or its confirmation/name dialogs. CameraService separately
    // suppresses host-camera playback while RaceSex Menu is open. Restore the
    // follower lock as soon as the native menu closes.
    const auto* pUI = UI::Get();

    const bool localSaveMade = CheckpointSaves::TakeLocalSaveMade();
    if (localSaveMade && m_isLeader && m_sessionState >= 2)
    {
        CheckpointSaves::WriteCarryHome(); // only a promoted follower has a home source
        CheckpointSaveRequest request{};
        request.CheckpointId = CheckpointSaves::NewCheckpointId(m_startEpoch);
        m_saveRoster = CaptureRosterPosition();
        m_saveRosterId = request.CheckpointId;
        m_transport.Send(request);
        spdlog::info("Leader saved: announcing checkpoint {}", request.CheckpointId);
    }
    // A checkpoint the leader announced: start it once this PC is in the world (the engine
    // defers a queued save until saving is allowed), then copy the save when it is on disk.
    // Only once the player has been in one cell for 5 s with no load screen: the leader's autosave at a load door
    // announces the checkpoint as it goes through, and saving as soon as the new cell had 3D recorded escorts the
    // engine had not yet brought through (Ralof outside Helgen Keep, 2026-09-30: missing after loading it).
    if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell)
    {
        // Outdoors the grid cell changes as the player walks: key on the worldspace there, on the cell indoors.
        const auto* pWorld = pPlayer->parentCell->worldspace;
        const uint32_t place = pWorld ? reinterpret_cast<const TESForm*>(pWorld)->formID : pPlayer->parentCell->formID;
        if (place != m_checkpointCellId)
        {
            m_checkpointCellId = place;
            m_checkpointCellSinceMs = GetTickCount64();
        }
    }
    else
        m_checkpointCellId = 0;
    if (!m_pendingCheckpoint.empty() && m_sessionState >= 2)
    {
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell && pPlayer->GetNiNode() &&
            m_checkpointCellId && GetTickCount64() - m_checkpointCellSinceMs >= 5000 &&
            (!pUI || !pUI->GetMenuOpen(BSFixedString("Loading Menu"))))
        {
            CheckpointSaves::WriteCarryHome();
            if (!m_isLeader)
                if (auto position = CaptureRosterPosition())
                    SendRosterPosition(m_pendingCheckpoint, *position);
            CheckpointSaves::Begin(m_pendingCheckpoint, m_isLeader);
            m_pendingCheckpoint.clear();
        }
    }
    CheckpointSaves::Poll();
    if (m_handoffTargetSteamId && m_isLeader && m_handoffCheckpointId.empty() &&
        (!m_handoffSaveRequested || GetTickCount64() - m_handoffSaveRequestedAtMs > 30000) &&
        GetTickCount64() >= m_handoffNextSaveAttemptMs)
    {
        m_handoffNextSaveAttemptMs = GetTickCount64() + 1000;
        m_handoffSaveRequested = CheckpointSaves::RequestNativeSave();
        if (m_handoffSaveRequested) m_handoffSaveRequestedAtMs = GetTickCount64();
    }
    if (m_handoffTargetSteamId && !m_handoffReady &&
        !m_handoffCheckpointId.empty() && GetTickCount64() >= m_handoffNextAckAttemptMs)
    {
        m_handoffNextAckAttemptMs = GetTickCount64() + 1000;
        if (m_checkpointRosters.contains(m_handoffCheckpointId) && CheckpointSaves::Has(m_handoffCheckpointId) &&
            (m_isLeader || CheckpointSaves::CarryHomeReady()))
        {
            RequestHandoffAck ack;
            ack.CheckpointId = m_handoffCheckpointId;
            m_transport.Send(ack);
            m_handoffAckSent = true;
            spdlog::info("Handoff: durable checkpoint {} acknowledged", ack.CheckpointId);
        }
    }

    // A save can carry a follower's first-person graph still in the scripted walking camera
    // (the stage-160 checkpoint did), and the load restores the bob. Leave it once per load by
    // playing its end idle (IdleWalkingCameraEnd, 0x10C00D), which the graph ignores otherwise.
    if (m_inParty && !m_isLeader && m_sessionState >= 2)
    {
        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer || !pPlayer->parentCell || !pPlayer->GetNiNode())
        {
            m_player3DSince = {};
            m_walkingCameraCleared = false;
        }
        else if (m_player3DSince == std::chrono::steady_clock::time_point{})
            m_player3DSince = std::chrono::steady_clock::now();
        else if (!m_walkingCameraCleared && std::chrono::steady_clock::now() - m_player3DSince > std::chrono::seconds(2))
        {
            m_walkingCameraCleared = true;
            if (auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(0x10C00D)))
                spdlog::info("Follower: cleared any saved walking camera (IdleWalkingCameraEnd played={})", pPlayer->PlayIdle(pIdle));
        }
    }

    const bool creatorOpen = pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"));
    if (m_sessionState == 2 && creatorOpen)
        m_creatorSeen = true;
    const bool releaseCreatorInput = m_inParty && !m_isLeader &&
        m_sessionState == 2 && m_startEpoch != 0 &&
        !m_worldGateHeld && !m_waitingForWorldReady && creatorOpen &&
        !m_world.GetOverlayService().GetActive();
    if (releaseCreatorInput != m_creatorInputReleased)
    {
        if (auto* pControls = PlayerControls::GetInstance())
        {
            // Released for the creator; not blocked again when it closes (that locked the follower's look
            // on the walk to the block).
            if (releaseCreatorInput)
                pControls->SetBlockPlayerInput(false);
            m_creatorInputReleased = releaseCreatorInput;
            spdlog::info("Follower character-creator input {} for epoch {}",
                releaseCreatorInput ? "released" : "gated", m_startEpoch);
        }
    }

    // The loading barrier only starts the host-led cinematic. A second,
    // server-confirmed barrier releases every follower together for gameplay.
    // The MQ101 threshold applies only to the vanilla New Game onboarding;
    // ordinary continued campaigns use the native loaded-cell/control state.
    // New Game: Done in the creator counts although the creator stays open (CreatorTogether holds it).
    const bool creatorDone = CreatorTogether::IsDone();
    if (m_inParty && m_campaignMode == 1 && m_sessionState == 2 &&
        m_gameplayReadySent && creatorOpen && !creatorDone)
    {
        PartyReadyRequest request;
        request.Ready = false;
        m_transport.Send(request);
        m_gameplayReadySent = false;
        spdlog::info("Character creator: withdrew gameplay readiness for epoch {}", m_startEpoch);
    }
    if (m_inParty && m_sessionState == 2 && !m_gameplayReadySent &&
        !m_worldGateHeld && !m_waitingForWorldReady && pUI &&
        (!creatorOpen || creatorDone) && !pUI->GetMenuOpen(BSFixedString("Loading Menu")))
    {
        auto* pPlayer = PlayerCharacter::Get();
        auto* pControls = PlayerControls::GetInstance();
        const bool loaded = pPlayer && pPlayer->parentCell;
        const bool nativeControl = pControls && pControls->pMovementHandler &&
            pControls->pMovementHandler->isEnabled && pControls->pLookHandler &&
            pControls->pLookHandler->isEnabled;
        bool ready = m_campaignMode != 1 && loaded && nativeControl;
        // New Game: a player (host or follower) is ready once it closed the character creator. The
        // host used to count only when the intro had moved on with control back, so it simply
        // carried on while the followers were still editing.
        if (m_campaignMode == 1 && loaded)
            ready = m_creatorSeen && (!CreatorTogether::SkipRequested() || CreatorTogether::SkipReady());
        if (CreatorTogether::SkipRequested() && !CreatorTogether::SkipReady())
            ready = false;
        if (ready)
        {
            PartyReadyRequest request;
            request.Ready = true;
            m_transport.Send(request);
            m_gameplayReadySent = true;
            spdlog::info("Reached shared-campaign gameplay barrier for epoch {}", m_startEpoch);
        }
    }

    // Character creation together: hold Done until every player is done, then close together. Continue counts too: a
    // shared checkpoint from before the Helgen creator (the cart-exit save) reached the creator with this off, so the
    // vanilla menu showed the other player's copy standing in the same spot (owner: "double characters").
    // A continued session is already in play (state 3) when a checkpoint before the creator reaches it, so it runs
    // the creator on its own: others hidden, Done held until every other player is done, then close together.
    const bool continuedCreator = m_inParty && m_campaignMode == PartyStartRequest::kContinue && m_sessionState >= 2;
    CreatorTogether::Update(m_world, (m_inParty && m_campaignMode == PartyStartRequest::kNew && m_sessionState == 2) ||
        continuedCreator, creatorOpen);
    if (m_sessionState >= 3 && m_campaignMode != PartyStartRequest::kContinue)
        CreatorTogether::Release(true);
    if (continuedCreator && creatorOpen && CreatorTogether::IsDone() && CreatorTogether::OthersDone())
        CreatorTogether::Release(true);

    // Cutscene follow: until the leader is free, the scene plays once, the leader's way (not during
    // the character creator, which CreatorTogether handles).
    {
        const bool leaderFree = m_isLeader ? m_leaderFreeSent == 1 : m_leaderFree;
        bool canFollow = !m_world.GetDoorVoteService().HasPendingVote();
        if (canFollow && !m_isLeader && m_inParty && !leaderFree)
        {
            // A scene starting beyond a load door must not pull a follower across its boundary.
            auto* player = PlayerCharacter::Get();
            auto* cell = player ? player->GetParentCellEx() : nullptr;
            canFollow = false;
            auto view = m_world.view<FormIdComponent, PlayerComponent>();
            for (auto entity : view)
            {
                if (view.get<PlayerComponent>(entity).Id != m_leaderPlayerId)
                    continue;
                auto* leader = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                auto* leaderCell = leader ? leader->GetParentCellEx() : nullptr;
                canFollow = DoorVotePolicy::CanFollow(false, reinterpret_cast<uintptr_t>(cell),
                    reinterpret_cast<uintptr_t>(leaderCell), reinterpret_cast<uintptr_t>(cell ? cell->worldspace : nullptr),
                    reinterpret_cast<uintptr_t>(leaderCell ? leaderCell->worldspace : nullptr));
                break;
            }
        }
        const bool cutscene = m_inParty && m_sessionState >= 2 && m_startEpoch != 0 && !leaderFree && !creatorOpen && canFollow;
        CutsceneFollow::Update(m_world, cutscene, m_isLeader, m_leaderPlayerId);
    }

    // Leader: tell the party whether this character is free (no intro or cutscene holding it), on
    // every change and every 5 s. Players pass through each other until it is.
    if (m_inParty && m_isLeader && m_sessionState >= 2)
    {
        PlayerControlState state;
        // Down, fallen or dead is not a cutscene: bleedout takes the leader's controls, and reporting that as "not
        // free" put every follower into cutscene follow, whose player then took the downed leader's pose and camera
        // (2026-09-30: the follower's character mangled, third person, glued to the fallen host). ReviveService owns
        // downed players.
        auto* pLocal = PlayerCharacter::Get();
        const uint32_t life = pLocal ? (pLocal->actorState.flags1 >> 21) & 0xF : 0;
        const bool down = pLocal && (pLocal->actorState.IsBleedingOut() || life == 1 || life == 2 || life == 7 || life == 8 ||
            m_world.GetReviveService().IsFallen(m_world.GetTransport().GetLocalPlayerId()));
        const bool free = m_sessionState >= 3 && (down || (CapturePlayerControlState(state) && state.Free));
        const auto nowMs = GetTickCount64();
        if (static_cast<int>(free) != m_leaderFreeSent || nowMs >= m_nextLeaderFreeSendMs)
        {
            if (static_cast<int>(free) != m_leaderFreeSent)
                spdlog::info("Leader free control: {}", free);
            m_leaderFreeSent = static_cast<int>(free);
            m_nextLeaderFreeSendMs = nowMs + 5000;
            LeaderControlRequest request;
            request.FreeControl = free;
            m_transport.Send(request);
        }
    }

    s_playerControlSync->Update();

    // Character creator together: a player who finished waits, held in place (it can still look
    // around at the others, shown in front of it), until every player finished.
    {
        const bool holdForCreator = m_inParty && m_campaignMode == 1 && m_sessionState == 2 && m_gameplayReadySent && !creatorOpen;
        auto* pControls = PlayerControls::GetInstance();
        if (holdForCreator != m_creatorWaitHeld && pControls && pControls->pMovementHandler)
        {
            pControls->pMovementHandler->isEnabled = !holdForCreator;
            m_creatorWaitHeld = holdForCreator;
            spdlog::info("Character creator: {} for the other players", holdForCreator ? "waiting" : "everyone finished, released");
        }
        if (m_creatorWaitHeld && GetTickCount64() >= m_nextCreatorWaitNoticeMs)
        {
            m_nextCreatorWaitNoticeMs = GetTickCount64() + 8000;
            // SendHUDMessage::ShowHUDMessage (ID 52933): the top-left notification.
            using TShowHUDMessage = void(const char*, const char*, bool);
            POINTER_SKYRIMSE(TShowHUDMessage, s_showHUDMessage, 52933);
            s_showHUDMessage.Get()("Waiting for the other players to finish their characters...", nullptr, true);
        }
    }

    // New Game does not consistently emit TESLoadGameEvent. Treat the first
    // live player cell after the title menu closes as the equivalent boundary.
    if (m_inParty && m_sessionState >= 1 && m_homeApplyState == 1)
    {
        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer || !pPlayer->parentCell || !pPlayer->GetNiNode() || !pUI ||
            pUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
            pUI->GetMenuOpen(BSFixedString("Main Menu")) || LobbyService::InLobbyWorld())
            m_homeWorldSince = 0;
        else if (!m_homeWorldSince)
            m_homeWorldSince = GetTickCount64();
        else if (GetTickCount64() - m_homeWorldSince >= 3000)
        {
            auto* pIntro = Cast<TESQuest>(TESForm::GetById(0x3372B));
            if (m_campaignMode == PartyStartRequest::kContinue && pIntro && !pIntro->IsStageDone(80))
            {
                // CreatorTogether owns appearance at stage 75 and state at Keep unbinding.
                // A continued pre-creator checkpoint must reach world-ready without this
                // competing full apply copying level and gear into the prisoner phase.
                m_homeApplyState = 0;
                spdlog::info("Follower: pre-creator checkpoint defers lobby character to creator skip and Keep unbinding");
            }
            else
            {
                CharacterSnapshot home;
                if (CharacterSnapshots::ReadFile(m_homeSourceSnap, home))
                {
                    CharacterSnapshots::QueueApply(std::move(home));
                    m_homeApplyState = 2;
                    spdlog::info("Follower: applying lobby character from {} before joining the shared world",
                        m_homeSourceSnap.filename().string());
                }
                else
                {
                    spdlog::error("Follower: lobby character {} could not be read; carry-home disabled", m_homeSourceSnap.string());
                    m_homeApplyState = 0;
                }
            }
        }
    }
    else if (m_inParty && m_sessionState >= 1 && m_homeApplyState == 2)
    {
        const auto status = CharacterSnapshots::ApplyStatus();
        if (status.rfind("done", 0) == 0)
        {
            CheckpointSaves::SetCarryHomeSource(m_homeSourceKey);
            m_homeApplyState = 3;
            spdlog::info("Follower: lobby character applied; carry-home enabled for {}", m_homeSourceKey);
        }
        else if (status.rfind("failed", 0) == 0)
        {
            spdlog::error("Follower: lobby character apply failed; carry-home disabled: {}", status);
            m_homeApplyState = 0;
        }
    }
    if (m_waitingForWorldReady && !m_worldGateHeld)
    {
        auto* pPlayer = PlayerCharacter::Get();
        auto* pUI = UI::Get();
        // The live lobby's throwaway world is in a cell with no Main Menu open too: it is not the shared world.
        if (m_homeApplyState != 1 && m_homeApplyState != 2 && pPlayer && pPlayer->parentCell && pUI &&
            !pUI->GetMenuOpen(BSFixedString("Main Menu")) && !LobbyService::InLobbyWorld())
            ReachWorldReadyBarrier();
    }

    const auto cCurrentTick = m_transport.GetClock().GetCurrentTick();
    if (m_nextUpdate > cCurrentTick)
        return;

    // Update once every second
    m_nextUpdate = cCurrentTick + 1000;

    auto itor = std::begin(m_invitations);
    while (itor != std::end(m_invitations))
    {
        if (itor->second < cCurrentTick)
            itor = m_invitations.erase(itor);
        else
            ++itor;
    }
}

void PartyService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    if (m_inParty && m_partySteamIds.size() > 1 && !m_handoffDeparted && !m_handoffTargetSteamId)
    {
        const auto oldLeader = std::find(m_partyMembers.begin(), m_partyMembers.end(), m_leaderPlayerId);
        if (oldLeader != m_partyMembers.end())
        {
            const size_t oldIndex = static_cast<size_t>(oldLeader - m_partyMembers.begin());
            const uint64_t successor = HandoffPolicy::Successor(m_partySteamIds, m_partySteamIds[oldIndex]);
            m_handoffOrder.clear();
            m_handoffOrder.push_back(successor);
            for (const auto id : m_partySteamIds)
                if (id != successor) m_handoffOrder.push_back(id);
            m_handoffSeedCheckpoint = m_currentCheckpoint;
            m_handoffDeparted = true;
            m_handoffAvailabilitySent = false;
            m_world.GetSteamLobbyService().BeginHandoff(successor, m_handoffOrder, true);
            spdlog::info("Handoff: server lost; successor Steam {} elected", successor);
        }
    }
    if (m_inParty && m_sessionState >= 2)
    {
        CheckpointSaves::WriteCarryHome();
        LobbyService::RequestReturnToLobby("disconnected");
    }
    DestroyParty();
}

void PartyService::OnPlayerList(const NotifyPlayerList& acPlayerList) noexcept
{
    m_players = acPlayerList.Players;
}

void PartyService::OnPartyInfo(const NotifyPartyInfo& acPartyInfo) noexcept
{
    if (m_inParty)
    {
        spdlog::debug("[PartyService]: Got party info update");
        m_isLeader = acPartyInfo.IsLeader;
        m_leaderPlayerId = acPartyInfo.LeaderPlayerId;
        m_partyMembers = acPartyInfo.PlayerIds;
        m_partySteamIds = acPartyInfo.SteamIds;
        if (m_partySteamIds.size() == m_partyMembers.size())
        {
            const auto leader = std::find(m_partyMembers.begin(), m_partyMembers.end(), m_leaderPlayerId);
            const auto index = leader == m_partyMembers.end() ? 0 : static_cast<size_t>(leader - m_partyMembers.begin());
            m_world.GetSteamLobbyService().RememberParty(m_partySteamIds, m_partySteamIds[index]);
        }
        if (acPartyInfo.HandoffTargetSteamId && !m_handoffTargetSteamId)
        {
            m_handoffTargetSteamId = acPartyInfo.HandoffTargetSteamId;
            m_handoffOrder.clear();
            m_handoffOrder.push_back(m_handoffTargetSteamId);
            for (const auto id : m_partySteamIds)
                if (id != m_handoffTargetSteamId) m_handoffOrder.push_back(id);
            if (m_isLeader && !m_handoffSaveRequested)
            {
                m_handoffSaveRequested = CheckpointSaves::RequestNativeSave();
                if (m_handoffSaveRequested) m_handoffSaveRequestedAtMs = GetTickCount64();
                spdlog::info("Handoff: leader checkpoint save queued={}", m_handoffSaveRequested);
            }
        }
        if (!acPartyInfo.HandoffTargetSteamId && m_handoffTargetSteamId && !m_handoffDeparted)
        {
            m_handoffTargetSteamId = 0;
            m_handoffCheckpointId.clear();
            m_handoffOrder.clear();
            m_handoffSaveRequested = false;
            m_handoffAckSent = false;
            spdlog::warn("Handoff: successor left before the checkpoint barrier; request again to retry");
        }
        if (!acPartyInfo.HandoffCheckpointId.empty() && m_handoffCheckpointId != acPartyInfo.HandoffCheckpointId)
        {
            m_handoffCheckpointId = acPartyInfo.HandoffCheckpointId;
            m_handoffAckSent = false;
        }
        m_handoffReady = acPartyInfo.HandoffReady;
        m_readyPlayers = acPartyInfo.ReadyPlayerIds;
        m_campaignMode = acPartyInfo.CampaignMode;
        if (!acPartyInfo.CheckpointId.empty() && m_campaignMode == PartyStartRequest::kContinue)
            m_currentCheckpoint = acPartyInfo.CheckpointId;
        if (acPartyInfo.HandoffSeeded)
        {
            m_handoffCampaign = true;
            m_handoffTargetSteamId = 0;
            if (!acPartyInfo.CheckpointId.empty()) m_currentCheckpoint = acPartyInfo.CheckpointId;
        }
        const auto previousSessionState = m_sessionState;
        m_sessionState = acPartyInfo.SessionState;
        m_startEpoch = acPartyInfo.StartEpoch;
        m_leaderGeneration = acPartyInfo.LeaderGeneration;
        if (previousSessionState == 0 && m_sessionState == 1 && !m_isLeader)
        {
            CheckpointSaves::SetCarryHomeSource({});
            m_homeSourceKey = LobbyService::ChosenOwnPlaythroughKey();
            m_homeSourceSnap = LobbyService::ChosenOwnSnapshotPath();
            // New Game's vanilla intro strips inventory. The chosen character is restored by
            // CreatorTogether at the creator and Keep unbinding, never at world load.
            m_homeApplyState = m_campaignMode == PartyStartRequest::kContinue &&
                !m_homeSourceKey.empty() && !m_homeSourceSnap.empty() ? 1 : 0;
            m_homeWorldSince = 0;
        }
        RefreshFollowerIntroProtection();

        // TODO: this can be done a bit prettier
        if (m_isLeader)
        {
            TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1));
            pWorldEncountersEnabled->f = 1.f;
        }

        auto pArguments = CefListValue::Create();

        auto pPlayerIds = CefListValue::Create();
        for (int i = 0; i < m_partyMembers.size(); i++)
            pPlayerIds->SetInt(i, m_partyMembers[i]);

        pArguments->SetList(0, pPlayerIds);
        pArguments->SetInt(1, acPartyInfo.LeaderPlayerId);
        auto pReadyIds = CefListValue::Create();
        for (int i = 0; i < m_readyPlayers.size(); ++i)
            pReadyIds->SetInt(i, m_readyPlayers[i]);
        pArguments->SetList(2, pReadyIds);
        pArguments->SetInt(3, acPartyInfo.CampaignMode);
        pArguments->SetInt(4, acPartyInfo.SessionState);
        pArguments->SetString(5, std::to_string(acPartyInfo.StartEpoch));
        pArguments->SetString(6, acPartyInfo.CheckpointId.c_str());
        pArguments->SetBool(7, acPartyInfo.LobbyOpen);
        pArguments->SetBool(8, acPartyInfo.PasswordProtected);
        // This player's id on the server it is connected to now. The overlay learned it once, at the first connect: a
        // player who joined a friend's session kept the id of their own one-player session, so after a promotion their
        // CO-OP screen never saw them as the host and offered no "Make host" (owner 2026-10-02).
        pArguments->SetInt(9, static_cast<int>(m_transport.GetLocalPlayerId()));

        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyInfo", pArguments);
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("coopLobbyState", pArguments);
        m_world.GetSteamLobbyService().ApplyPartySettings(acPartyInfo.LobbyOpen, acPartyInfo.PasswordProtected);
        if (m_handoffDeparted && m_isLeader && m_sessionState == 0 &&
            !acPartyInfo.CheckpointId.empty() && acPartyInfo.CheckpointId != m_handoffSeedCheckpoint)
            SendHandoffSeed(acPartyInfo.CheckpointId);
        if (m_handoffDeparted && acPartyInfo.HandoffSeeded && !m_handoffAvailabilitySent)
        {
            RequestCheckpointAvailability available;
            available.Checkpoints = CheckpointSaves::Available(32);
            m_transport.Send(available);
            m_handoffAvailabilitySent = true;
        }
        if (m_handoffDeparted && acPartyInfo.HandoffSeeded && m_sessionState >= 2)
        {
            m_handoffDeparted = false;
            m_handoffOrder.clear();
            m_handoffAckSent = false;
            m_handoffSaveRequested = false;
            m_handoffCheckpointId.clear();
        }
        if (m_handoffReady && !m_handoffDeparted && !m_handoffOrder.empty())
        {
            m_handoffSeedCheckpoint = m_handoffCheckpointId;
            m_handoffDeparted = true;
            m_handoffAvailabilitySent = false;
            LobbyService::RequestReturnToLobby("party_left");
            m_world.GetSteamLobbyService().BeginHandoff(m_handoffTargetSteamId, m_handoffOrder);
        }
        // A wiped party reloads its checkpoint in game, as vanilla reloads the last save on death: forget every tracked
        // actor without releasing ownership (the leader keeps its NPCs), then load; the leader first, followers once
        // the leader's world is in.
        const bool restart = previousSessionState >= 2 && m_sessionState == 1 && m_world.GetReviveService().IsWiped();
        if (restart)
        {
            // Owner (2026-09-28): straight to black, no standing around while the leader loads; black holds through
            // the load (reapplied after it) until the gameplay barrier releases everyone together.
            if (MT::CutEnabled(6))
                m_restartFadePending.store(true, std::memory_order_release);
            else
                FadeOutGame(true, true, 0.5f, true, 0.f);
            m_wipeFade = true;
            m_wipeFadeSince = GetTickCount64();
            m_reloadPending = true;
            m_world.GetCharacterService().ReleaseForReload();
            spdlog::info("Party wipe: reloading checkpoint '{}' in game", acPartyInfo.CheckpointId);
        }
        if ((previousSessionState == 0 || restart) && m_sessionState == 1)
        {
            m_creatorSeen = false;
            m_gameplayReadySent = false;
            // Return control to Skyrim's main-menu movie before asking it to
            // run the native NEW/CONTINUE confirmation path. Leaving CEF
            // active hides that state transition and keeps its input hook.
            m_world.GetOverlayService().SetActive(false);
            m_waitingForWorldReady = true;
            // A follower's Continue waits for the leader's world to load first (the leader registers its actors
            // before anyone else's copies arrive).
            const bool leaderLoaded = std::find(acPartyInfo.ReadyPlayerIds.begin(), acPartyInfo.ReadyPlayerIds.end(),
                m_leaderPlayerId) != acPartyInfo.ReadyPlayerIds.end();
            if (m_campaignMode == PartyStartRequest::kContinue && !m_isLeader && !leaderLoaded)
            {
                m_deferredCheckpoint = acPartyInfo.CheckpointId;
                m_deferredLaunch = true;
                m_deferredSince = GetTickCount64();
                spdlog::info("Continue: waiting for the leader to load first");
            }
            else
                LaunchCheckpoint(acPartyInfo.CheckpointId);
        }
        else if (m_deferredLaunch && m_sessionState == 1 &&
            std::find(acPartyInfo.ReadyPlayerIds.begin(), acPartyInfo.ReadyPlayerIds.end(), m_leaderPlayerId) !=
                acPartyInfo.ReadyPlayerIds.end())
        {
            spdlog::info("Continue: the leader has loaded after {} ms, loading now", GetTickCount64() - m_deferredSince);
            m_deferredLaunch = false;
            LaunchCheckpoint(m_deferredCheckpoint);
        }
        else if (previousSessionState == 1 && m_sessionState == 2)
        {
            if (m_worldGateHeld)
            {
                if (auto* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
                    --pUI->numPausesGame;
                PlayerControls::QueueInputRelease();
                m_worldGateHeld = false;
            }
            m_waitingForWorldReady = false;
            // The leader drives the intro. A follower is not input-locked for it: the global block also
            // kills looking, and the host can look around the whole intro (MQ101 leaves the look channel on).
            // Cutscene follow disables the follower's movement, and its own copy of the intro's scripts
            // disables the same control channels the host's do.
            if (auto* pControls = PlayerControls::GetInstance())
                pControls->SetBlockPlayerInput(false);
            spdlog::info("Shared-campaign world-ready barrier released for epoch {}", m_startEpoch);
            // Everyone has loaded: fade in together here. The gameplay barrier can be minutes away (a fresh intro
            // reaches it only after the character creator): waiting for it kept both screens black through the whole
            // cart ride until the 3-minute lift (session 2026-09-29 22:14-22:17).
            if (m_wipeFade)
            {
                FadeOutGame(false, true, 2.5f, true, 0.f);
                m_wipeFade = false;
            }
        }
        else if (previousSessionState == 2 && m_sessionState == 3)
        {
            if (auto* pControls = PlayerControls::GetInstance(); pControls && !m_isLeader)
                pControls->SetBlockPlayerInput(false);
            m_creatorInputReleased = false;
            RefreshFollowerIntroProtection();
            spdlog::info("Shared-campaign gameplay barrier released for epoch {}", m_startEpoch);
            if (m_wipeFade)
            {
                // Everyone is in and synced: fade in together, slowly, like the game's opening.
                FadeOutGame(false, true, 2.5f, true, 0.f);
                m_wipeFade = false;
            }
        }
    }
}

void PartyService::OnPartyInvite(const NotifyPartyInvite& acPartyInvite) noexcept
{
    spdlog::debug("[PartyService]: Got party invite from {}", acPartyInvite.InviterId);

    m_invitations[acPartyInvite.InviterId] = acPartyInvite.ExpiryTick;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acPartyInvite.InviterId);
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyInviteReceived", pArguments);
}

void PartyService::OnPartyJoined(const NotifyPartyJoined& acPartyJoined) noexcept
{
    spdlog::debug("[PartyService]: Joined party. LeaderId: {}, IsLeader: {}", acPartyJoined.LeaderPlayerId, acPartyJoined.IsLeader);

    m_inParty = true;
    m_isLeader = acPartyJoined.IsLeader;
    m_leaderPlayerId = acPartyJoined.LeaderPlayerId;
    m_partyMembers = acPartyJoined.PlayerIds;
    if (m_handoffDeparted)
    {
        m_handoffTargetSteamId = 0;
        m_handoffCheckpointId.clear();
        m_handoffReady = false;
        m_handoffAckSent = false;
        m_handoffSaveRequested = false;
        const auto localSteam = m_world.GetSteamLobbyService().LocalSteamId();
        if (!m_handoffOrder.empty() && m_handoffOrder.front() == localSteam)
        {
            SendHandoffSeed(m_handoffSeedCheckpoint);
            RequestCheckpointAvailability available;
            available.Checkpoints = CheckpointSaves::Available(32);
            m_transport.Send(available);
            m_handoffAvailabilitySent = true;
        }
    }
    m_world.GetSteamLobbyService().FinishHandoff();

    m_world.GetDispatcher().trigger(PartyJoinedEvent(m_isLeader));
}

void PartyService::OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept
{
    spdlog::debug("[PartyService]: Left party");
    m_world.GetSteamLobbyService().ForgetParty();

    if (!acPartyLeft.Forced && m_inParty && m_sessionState >= 2)
    {
        CheckpointSaves::WriteCarryHome();
        LobbyService::RequestReturnToLobby("party_left"); // a forced leave quits to the menu, which returns there too
    }

    const bool forced = acPartyLeft.Forced;
    const bool needsCarry = forced && m_inParty && m_sessionState >= 2;
    DestroyParty();

    if (forced)
        m_forcedLeavePending.store(needsCarry ? 2 : 1);
    m_world.GetDispatcher().trigger(PartyLeftEvent());
}

void PartyService::DestroyParty() noexcept
{
    if (!m_handoffDeparted)
    {
        m_handoffCampaign = false;
        m_handoffOrder.clear();
        m_checkpointRosters.clear();
        m_rosterOrder.clear();
        m_handoffSeedCheckpoint.clear();
        m_handoffTargetSteamId = 0;
        m_handoffCheckpointId.clear();
        m_handoffSaveRequested = false;
        m_handoffAckSent = false;
        m_handoffReady = false;
    }
    m_homeApplyState = 0;
    m_homeSourceKey.clear();
    m_homeSourceSnap.clear();
    m_homeWorldSince = 0;
    if (m_worldGateHeld)
    {
        if (auto* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
            --pUI->numPausesGame;
        PlayerControls::QueueInputRelease();
        m_worldGateHeld = false;
    }
    if (m_waitingForWorldReady || m_creatorInputReleased ||
        (m_inParty && !m_isLeader && m_sessionState >= 1))
    {
        if (auto* pControls = PlayerControls::GetInstance())
            pControls->SetBlockPlayerInput(false);
    }
    m_waitingForWorldReady = false;
    ReleaseFollowerIntroProtection();
    m_creatorInputReleased = false;
    m_creatorSeen = false;
    m_gameplayReadySent = false;
    m_inParty = false;
    m_isLeader = false;
    m_leaderPlayerId = -1;
    m_partyMembers.clear();
    m_partySteamIds.clear();
    m_readyPlayers.clear();
    m_campaignMode = 0;
    m_currentCheckpoint.clear();
    m_sessionState = 0;
    m_startEpoch = 0;
    m_leaderGeneration = 0;
}

void PartyService::RefreshFollowerIntroProtection() noexcept
{
    const bool shouldProtect = m_inParty && !m_isLeader &&
        m_sessionState >= 1 && m_sessionState <= 2;
    if (!shouldProtect)
    {
        ReleaseFollowerIntroProtection();
        return;
    }

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (!m_followerIntroProtectionHeld)
    {
        m_playerWasEssential = pPlayer->IsEssential();
        m_followerIntroProtectionHeld = true;
        spdlog::info("Follower intro protection enabled for epoch {}", m_startEpoch);
    }

    pPlayer->SetEssentialEx(true);
    pPlayer->SetNoBleedoutRecovery(false);

    // Recover a follower that was killed during the load boundary before this
    // guard obtained a live PlayerCharacter. Resurrect without resetting the
    // inventory or reference so the shared campaign state remains intact.
    if (pPlayer->IsDead())
    {
        spdlog::warn("Recovered follower killed by local intro physics");
        pPlayer->Resurrect(false);
    }

    const float maxHealth = pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth);
    if (pPlayer->GetActorValue(ActorValueInfo::kHealth) < maxHealth)
        pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, maxHealth);
}

void PartyService::ReleaseFollowerIntroProtection() noexcept
{
    if (!m_followerIntroProtectionHeld)
        return;

    if (auto* pPlayer = PlayerCharacter::Get())
    {
        pPlayer->SetNoBleedoutRecovery(false);
        pPlayer->SetEssentialEx(m_playerWasEssential);
    }

    m_followerIntroProtectionHeld = false;
    m_playerWasEssential = false;
    spdlog::info("Follower intro protection disabled");
}


// Follower: the leader's free control. Players pass through each other until the leader is free;
// the first time it is in a session, this player is placed around the leader to start playing.
void PartyService::OnNotifyLeaderControl(const NotifyLeaderControl& acMessage) noexcept
{
    m_leaderFree = acMessage.FreeControl;
    PlayerCollision::SetLeaderFreeControl(acMessage.FreeControl);
    if (!acMessage.FreeControl || m_gatheredAroundLeader || m_isLeader ||
        m_campaignMode == PartyStartRequest::kContinue || m_world.GetDropInService().PreserveReturnPosition())
        return;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    // Not while this player's own scene still holds it (seated, AI-driven, restrained): moving it then
    // pulled the follower off the chopping block, and its stand-up (IdleFurnitureExit) never played. The
    // leader resends free control every 5 s, so the gather happens once this player is free too.
    const uint32_t sitSleepState = (pPlayer->actorState.flags1 >> 14) & 0xF;
    PlayerControlState localState;
    if (!PlayerCollision::LocalHasFreeControl() || !CapturePlayerControlState(localState) || !localState.Free ||
        !s_playerControlSync->HasRelease() || !s_playerControlSync->Ready)
    {
        spdlog::info("Leader has free control; this player is still in its own scene (sit state {}), gathered later", sitSleepState);
        return;
    }
    // The leader's character here, and this player's slot among the followers.
    Actor* pLeader = nullptr;
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        if (view.get<PlayerComponent>(entity).Id != m_leaderPlayerId)
            continue;
        pLeader = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        break;
    }
    if (!pLeader || !pLeader->parentCell)
        return;
    auto* cell = pPlayer->GetParentCellEx();
    auto* leaderCell = pLeader->GetParentCellEx();
    const bool pendingVote = m_world.GetDoorVoteService().HasPendingVote();
    if (!DoorVotePolicy::CanFollow(pendingVote, reinterpret_cast<uintptr_t>(cell),
        reinterpret_cast<uintptr_t>(leaderCell), reinterpret_cast<uintptr_t>(cell ? cell->worldspace : nullptr),
        reinterpret_cast<uintptr_t>(leaderCell ? leaderCell->worldspace : nullptr)))
    {
        // Retire a delayed gather once a vote starts or the leader crosses a load boundary.
        // Cancellation must not resurrect it and undo walking away. F8 remains explicit recovery.
        if (cell && leaderCell)
            m_gatheredAroundLeader = true;
        spdlog::info("Door vote: automatic gather skipped ({}), source {:X}, leader {:X}",
            pendingVote ? "vote pending" : "different load area", cell ? cell->formID : 0, leaderCell ? leaderCell->formID : 0);
        return;
    }
    uint32_t slot = 0;
    for (const auto memberId : m_partyMembers)
    {
        if (memberId == m_leaderPlayerId)
            continue;
        if (memberId == m_transport.GetLocalPlayerId())
            break;
        ++slot;
    }
    m_gatheredAroundLeader = true;
    // A half circle behind the leader, 150 units away.
    const float angle = pLeader->rotation.z + static_cast<float>(TiltedPhoques::Pi) + (static_cast<float>(slot) - 0.5f) * 0.9f;
    NiPoint3 target = pLeader->position;
    target.x += std::sin(angle) * 150.f;
    target.y += std::cos(angle) * 150.f;
    pPlayer->MoveTo(pLeader->parentCell, target);
    spdlog::info("Leader has free control: this player placed around the leader (slot {})", slot);
}
