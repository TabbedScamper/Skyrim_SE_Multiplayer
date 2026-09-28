#include <Services/CutsceneFollow.h>
#include <Services/Generic/UnstuckReset.h>
#include <Services/CreatorTogether.h>
#include <Messages/LeaderControlRequest.h>
#include <Messages/NotifyLeaderControl.h>
#include <Messages/RequestPlayerControlState.h>
#include <Messages/NotifyPlayerControlState.h>
#include <Services/PlayerCollision.h>
#include <Services/PartyService.h>
#include <Services/DoorVoteService.h>
#include <Forms/TESObjectCELL.h>

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
#include <Services/CheckpointSaves.h>
#include <Forms/TESIdleForm.h>

#include <OverlayApp.hpp>

#include <Forms/TESGlobal.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Forms/TESQuest.h>
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
        if (stored != 0x80000000u)
            return; // A local engine state owns a saved mask; do not overwrite its recovery.
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

void PartyService::LeaveParty() const noexcept
{
    PartyLeaveRequest request;
    m_transport.Send(request);
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

void PartyService::ChangePartyLeader(const uint32_t aPlayerId) const noexcept
{
    PartyChangeLeaderRequest changeMessage;
    changeMessage.PartyMemberPlayerId = aPlayerId;
    m_transport.Send(changeMessage);
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
    request.CheckpointId = aMode == PartyStartRequest::kContinue && acCheckpointId.empty() ? CheckpointSaves::Latest() : acCheckpointId;
    m_transport.Send(request);
}

void PartyService::StartTogether(const uint8_t aMode, const String& acCheckpointId) const noexcept
{
    PartyStartRequest request;
    request.Mode = aMode;
    request.Launch = true;
    request.CheckpointId = aMode == PartyStartRequest::kContinue && acCheckpointId.empty() ? CheckpointSaves::Latest() : acCheckpointId;
    m_transport.Send(request);
}

void PartyService::SetSessionSettings(const bool aOpen, const String& acPassword) const noexcept
{
    if (!m_isLeader)
        return;
    PartySessionSettingsRequest request;
    request.Open = aOpen;
    request.Password = acPassword;
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

void PartyService::ReachWorldReadyBarrier() noexcept
{
    if (!m_waitingForWorldReady || m_sessionState != 1 || m_worldGateHeld)
        return;

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
    m_pendingCheckpoint = acMessage.CheckpointId;
}

void PartyService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
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
        CheckpointSaveRequest request{};
        request.CheckpointId = CheckpointSaves::NewCheckpointId(m_startEpoch);
        m_transport.Send(request);
        spdlog::info("Leader saved: announcing checkpoint {}", request.CheckpointId);
    }
    // A checkpoint the leader announced: start it once this PC is in the world (the engine
    // defers a queued save until saving is allowed), then copy the save when it is on disk.
    if (!m_pendingCheckpoint.empty() && m_sessionState >= 2)
    {
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell && pPlayer->GetNiNode())
        {
            CheckpointSaves::Begin(m_pendingCheckpoint);
            m_pendingCheckpoint.clear();
        }
    }
    CheckpointSaves::Poll();

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
            ready = m_creatorSeen;
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
        const bool free = m_sessionState >= 3 && CapturePlayerControlState(state) && state.Free;
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
    if (m_waitingForWorldReady && !m_worldGateHeld)
    {
        auto* pPlayer = PlayerCharacter::Get();
        auto* pUI = UI::Get();
        if (pPlayer && pPlayer->parentCell && pUI && !pUI->GetMenuOpen(BSFixedString("Main Menu")))
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
        m_readyPlayers = acPartyInfo.ReadyPlayerIds;
        m_campaignMode = acPartyInfo.CampaignMode;
        const auto previousSessionState = m_sessionState;
        m_sessionState = acPartyInfo.SessionState;
        m_startEpoch = acPartyInfo.StartEpoch;
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

        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyInfo", pArguments);
        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("coopLobbyState", pArguments);
        m_world.GetSteamLobbyService().ApplyPartySettings(acPartyInfo.LobbyOpen, acPartyInfo.PasswordProtected);
        if (previousSessionState == 0 && m_sessionState == 1)
        {
            m_creatorSeen = false;
            m_gameplayReadySent = false;
            // Return control to Skyrim's main-menu movie before asking it to
            // run the native NEW/CONTINUE confirmation path. Leaving CEF
            // active hides that state transition and keeps its input hook.
            m_world.GetOverlayService().SetActive(false);
            m_waitingForWorldReady = true;
            // Continue with a named checkpoint loads that save on every PC; without one (or if
            // this PC does not have it) fall back to the game's own Continue.
            if (m_campaignMode != PartyStartRequest::kContinue || acPartyInfo.CheckpointId.empty() ||
                !CheckpointSaves::Load(acPartyInfo.CheckpointId))
            {
                if (m_campaignMode == PartyStartRequest::kContinue)
                    spdlog::warn("Continue without a matched checkpoint ('{}'): loading this PC's last save", acPartyInfo.CheckpointId);
                LaunchSharedCampaignFromMainMenu(m_campaignMode);
            }
        }
        else if (previousSessionState == 1 && m_sessionState == 2)
        {
            if (m_worldGateHeld)
            {
                if (auto* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
                    --pUI->numPausesGame;
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
        }
        else if (previousSessionState == 2 && m_sessionState == 3)
        {
            if (auto* pControls = PlayerControls::GetInstance(); pControls && !m_isLeader)
                pControls->SetBlockPlayerInput(false);
            m_creatorInputReleased = false;
            RefreshFollowerIntroProtection();
            spdlog::info("Shared-campaign gameplay barrier released for epoch {}", m_startEpoch);
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

    m_world.GetDispatcher().trigger(PartyJoinedEvent(m_isLeader));
}

void PartyService::OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept
{
    spdlog::debug("[PartyService]: Left party");

    DestroyParty();

    m_world.GetDispatcher().trigger(PartyLeftEvent());
}

void PartyService::DestroyParty() noexcept
{
    if (m_worldGateHeld)
    {
        if (auto* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
            --pUI->numPausesGame;
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
    m_readyPlayers.clear();
    m_campaignMode = 0;
    m_sessionState = 0;
    m_startEpoch = 0;
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
    if (!acMessage.FreeControl || m_gatheredAroundLeader || m_isLeader)
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
