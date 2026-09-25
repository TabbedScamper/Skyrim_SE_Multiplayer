#include <Services/PartyService.h>

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

#include <OverlayApp.hpp>

#include <Forms/TESGlobal.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Forms/TESQuest.h>

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransportService) noexcept
    : m_world(aWorld)
    , m_transport(aTransportService)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&PartyService::OnDisconnected>(this);

    m_playerListConnection = aDispatcher.sink<NotifyPlayerList>().connect<&PartyService::OnPlayerList>(this);
    m_partyInfoConnection = aDispatcher.sink<NotifyPartyInfo>().connect<&PartyService::OnPartyInfo>(this);
    m_partyInviteConnection = aDispatcher.sink<NotifyPartyInvite>().connect<&PartyService::OnPartyInvite>(this);
    m_partyJoinedConnection = aDispatcher.sink<NotifyPartyJoined>().connect<&PartyService::OnPartyJoined>(this);
    m_partyLeftConnection = aDispatcher.sink<NotifyPartyLeft>().connect<&PartyService::OnPartyLeft>(this);
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
    request.CheckpointId = acCheckpointId;
    m_transport.Send(request);
}

void PartyService::StartTogether(const uint8_t aMode, const String& acCheckpointId) const noexcept
{
    PartyStartRequest request;
    request.Mode = aMode;
    request.Launch = true;
    request.CheckpointId = acCheckpointId;
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
            pControls->SetBlockPlayerInput(!releaseCreatorInput);
            m_creatorInputReleased = releaseCreatorInput;
            spdlog::info("Follower character-creator input {} for epoch {}",
                releaseCreatorInput ? "released" : "gated", m_startEpoch);
        }
    }

    // The loading barrier only starts the host-led cinematic. A second,
    // server-confirmed barrier releases every follower together for gameplay.
    // The MQ101 threshold applies only to the vanilla New Game onboarding;
    // ordinary continued campaigns use the native loaded-cell/control state.
    if (m_inParty && m_sessionState == 2 && !m_gameplayReadySent &&
        !m_worldGateHeld && !m_waitingForWorldReady && pUI &&
        !creatorOpen && !pUI->GetMenuOpen(BSFixedString("Loading Menu")))
    {
        auto* pPlayer = PlayerCharacter::Get();
        auto* pControls = PlayerControls::GetInstance();
        const bool loaded = pPlayer && pPlayer->parentCell;
        const bool nativeControl = pControls && pControls->pMovementHandler &&
            pControls->pMovementHandler->isEnabled && pControls->pLookHandler &&
            pControls->pLookHandler->isEnabled;
        bool ready = m_campaignMode != 1 && loaded && nativeControl;
        if (m_campaignMode == 1 && loaded)
        {
            if (m_isLeader)
            {
                const auto* pIntro = Cast<TESQuest>(TESForm::GetById(0x0003372B));
                ready = pIntro && pIntro->currentStage >= 160 && nativeControl;
            }
            else
                ready = m_creatorSeen;
        }
        if (ready)
        {
            PartyReadyRequest request;
            request.Ready = true;
            m_transport.Send(request);
            m_gameplayReadySent = true;
            spdlog::info("Reached shared-campaign gameplay barrier for epoch {}", m_startEpoch);
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
            LaunchSharedCampaignFromMainMenu(m_campaignMode);
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
            // The leader drives the intro. Followers remain input-locked until
            // the character-creation phase controller explicitly releases them.
            if (auto* pControls = PlayerControls::GetInstance())
                pControls->SetBlockPlayerInput(!m_isLeader);
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
