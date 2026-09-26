#include <Messages/LeaderControlRequest.h>
#include <Messages/NotifyLeaderControl.h>
#include <Messages/RequestPlayerControlState.h>
#include <Messages/NotifyPlayerControlState.h>
#include <Messages/RequestScriptedCamera.h>
#include <Messages/NotifyScriptedCamera.h>
#include <Services/PartyService.h>
#include <Components.h>
#include <GameServer.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

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
#include <Messages/NotifySettingsChange.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/RequestPartyUnstuck.h>
#include <Messages/NotifyPartyUnstuck.h>
#include <World.h>

#include <chrono>
#include <unordered_map>

namespace
{
struct PlayerControlRelay
{
    PlayerControlRelay(PartyService& aParty, entt::dispatcher& aDispatcher)
        : Party(aParty)
        , Connection(aDispatcher.sink<PacketEvent<RequestPlayerControlState>>().connect<&PlayerControlRelay::OnRequest>(this))
        , LeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PlayerControlRelay::OnLeave>(this))
    {
    }

    void OnLeave(const PlayerLeaveEvent& acEvent) { Last.erase(acEvent.pPlayer->GetId()); }

    void OnRequest(const PacketEvent<RequestPlayerControlState>& acPacket)
    {
        auto* pPlayer = acPacket.pPlayer;
        if (!pPlayer || !Party.IsPlayerLeader(pPlayer))
            return;
        const auto* pParty = Party.GetPlayerParty(pPlayer);
        const auto& state = acPacket.Packet.State;
        if (!pParty || pParty->SessionState < 2 || state.Epoch != pParty->StartEpoch ||
            !state.IsValid() || (state.Free && pParty->SessionState < 3))
            return;
        auto& last = Last[pPlayer->GetId()];
        if (last.first == state.Epoch && state.Sequence <= last.second)
            return;
        last = {state.Epoch, state.Sequence};
        NotifyPlayerControlState notify;
        notify.LeaderId = pPlayer->GetId();
        notify.State = state;
        for (auto* pMember : pParty->Members)
        {
            if (pMember && pMember != pPlayer)
                pMember->Send(notify);
        }
    }

    PartyService& Party;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> Last;
    entt::scoped_connection Connection, LeaveConnection;
};

struct ScriptedCameraRelay
{
    ScriptedCameraRelay(PartyService& aParty, entt::dispatcher& aDispatcher)
        : Party(aParty)
        , Connection(aDispatcher.sink<PacketEvent<RequestScriptedCamera>>().connect<&ScriptedCameraRelay::OnRequest>(this))
        , LeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&ScriptedCameraRelay::OnLeave>(this))
    {
    }

    void OnLeave(const PlayerLeaveEvent& acEvent) { Last.erase(acEvent.pPlayer->GetId()); }

    void OnRequest(const PacketEvent<RequestScriptedCamera>& acPacket)
    {
        auto* pPlayer = acPacket.pPlayer;
        if (!pPlayer || !Party.IsPlayerLeader(pPlayer))
            return;
        const auto* pParty = Party.GetPlayerParty(pPlayer);
        const auto& state = acPacket.Packet.State;
        if (!pParty || pParty->SessionState < 2 || state.Epoch != pParty->StartEpoch || !state.IsValid())
            return;
        auto& last = Last[pPlayer->GetId()];
        if (last.first == state.Epoch && state.Sequence <= last.second)
            return;
        last = {state.Epoch, state.Sequence};
        NotifyScriptedCamera notify;
        notify.LeaderId = pPlayer->GetId();
        notify.State = state;
        for (auto* pMember : pParty->Members)
        {
            if (pMember && pMember != pPlayer)
                pMember->Send(notify);
        }
    }

    PartyService& Party;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> Last;
    entt::scoped_connection Connection;
    entt::scoped_connection LeaveConnection;
};

struct PartyUnstuckRelay
{
    PartyUnstuckRelay(PartyService& aParty, entt::dispatcher& aDispatcher)
        : Party(aParty)
        , Connection(aDispatcher.sink<PacketEvent<RequestPartyUnstuck>>().connect<&PartyUnstuckRelay::OnRequest>(this))
        , LeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PartyUnstuckRelay::OnLeave>(this))
    {
    }

    void OnLeave(const PlayerLeaveEvent& acEvent)
    {
        // Connection IDs can be reused after a disconnect.
        Last.erase(acEvent.pPlayer->GetId());
    }

    void OnRequest(const PacketEvent<RequestPartyUnstuck>& acPacket)
    {
        auto* pPlayer = acPacket.pPlayer;
        if (!pPlayer || !Party.IsPlayerLeader(pPlayer))
            return;
        auto* pParty = Party.GetPlayerParty(pPlayer);
        const auto& move = acPacket.Packet.Move;
        if (!pParty || move.Epoch != pParty->StartEpoch || !move.IsValid() || !acPacket.Packet.State.IsValid(move))
            return;
        const auto now = std::chrono::steady_clock::now();
        auto& last = Last[pPlayer->GetId()];
        if (now < last.Next || (last.Epoch == move.Epoch && move.Sequence <= last.Sequence))
            return;
        last = {move.Epoch, move.Sequence, now + std::chrono::milliseconds(1500)};
        NotifyPartyUnstuck notify;
        notify.Move = move;
        notify.State = acPacket.Packet.State;
        notify.LeaderId = pPlayer->GetId();
        for (auto* pMember : pParty->Members)
        {
            if (pMember && pMember != pPlayer)
            {
                pMember->Send(notify);
                ++notify.Slot;
            }
        }
        spdlog::info("Unstuck: host brought {} players (move relayed, sequence={})", notify.Slot, move.Sequence);
    }

    struct Stamp
    {
        uint64_t Epoch{};
        uint64_t Sequence{};
        std::chrono::steady_clock::time_point Next{};
    };
    PartyService& Party;
    std::unordered_map<uint32_t, Stamp> Last;
    entt::scoped_connection Connection;
    entt::scoped_connection LeaveConnection;
};
}

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateEvent(aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&PartyService::OnPlayerJoin>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PartyService::OnPlayerLeave>(this))
    , m_partyInviteConnection(aDispatcher.sink<PacketEvent<PartyInviteRequest>>().connect<&PartyService::OnPartyInvite>(this))
    , m_partyAcceptInviteConnection(aDispatcher.sink<PacketEvent<PartyAcceptInviteRequest>>().connect<&PartyService::OnPartyAcceptInvite>(this))
    , m_partyLeaveConnection(aDispatcher.sink<PacketEvent<PartyLeaveRequest>>().connect<&PartyService::OnPartyLeave>(this))
    , m_partyCreateConnection(aDispatcher.sink<PacketEvent<PartyCreateRequest>>().connect<&PartyService::OnPartyCreate>(this))
    , m_partyChangeLeaderConnection(aDispatcher.sink<PacketEvent<PartyChangeLeaderRequest>>().connect<&PartyService::OnPartyChangeLeader>(this))
    , m_partyKickConnection(aDispatcher.sink<PacketEvent<PartyKickRequest>>().connect<&PartyService::OnPartyKick>(this))
    , m_partyReadyConnection(aDispatcher.sink<PacketEvent<PartyReadyRequest>>().connect<&PartyService::OnPartyReady>(this))
    , m_leaderControlConnection(aDispatcher.sink<PacketEvent<LeaderControlRequest>>().connect<&PartyService::OnLeaderControl>(this))
    , m_partyStartConnection(aDispatcher.sink<PacketEvent<PartyStartRequest>>().connect<&PartyService::OnPartyStart>(this))
    , m_partySessionSettingsConnection(aDispatcher.sink<PacketEvent<PartySessionSettingsRequest>>().connect<&PartyService::OnPartySessionSettings>(this))
    , m_partyGameplaySettingsConnection(aDispatcher.sink<PacketEvent<PartyGameplaySettingsRequest>>().connect<&PartyService::OnPartyGameplaySettings>(this))
    , m_checkpointSaveConnection(aDispatcher.sink<PacketEvent<CheckpointSaveRequest>>().connect<&PartyService::OnCheckpointSave>(this))
{
    // Its own storage: emplacing into the world context from inside this constructor (while the
    // context is still constructing this service) killed the server on startup.
    static std::unique_ptr<PartyUnstuckRelay> s_unstuckRelay;
    s_unstuckRelay = std::make_unique<PartyUnstuckRelay>(*this, aDispatcher);
    static std::unique_ptr<ScriptedCameraRelay> s_cameraRelay;
    s_cameraRelay = std::make_unique<ScriptedCameraRelay>(*this, aDispatcher);
    static std::unique_ptr<PlayerControlRelay> s_controlRelay;
    s_controlRelay = std::make_unique<PlayerControlRelay>(*this, aDispatcher);
}

const PartyService::Party* PartyService::GetById(uint32_t aId) const noexcept
{
    auto itor = m_parties.find(aId);
    if (itor != std::end(m_parties))
        return &itor->second;

    return nullptr;
}

bool PartyService::IsPlayerInParty(Player* const apPlayer) const noexcept
{
    return apPlayer->GetParty().JoinedPartyId.has_value();
}

bool PartyService::IsPlayerLeader(const Player* const apPlayer) const noexcept
{
    const auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        if (const auto* const pParty = GetById(*inviterPartyComponent.JoinedPartyId))
            return pParty->LeaderPlayerId == apPlayer->GetId();
    }

    return false;
}

PartyService::Party* PartyService::GetPlayerParty(Player* const apPlayer) noexcept
{
    auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        return &m_parties[*inviterPartyComponent.JoinedPartyId];
    }

    return nullptr;
}

ServerSettings PartyService::GetSettingsForPlayer(const Player* apPlayer) const noexcept
{
    auto settings = GetSettings();
    if (const auto partyId = apPlayer->GetParty().JoinedPartyId)
    {
        if (const auto* pParty = GetById(*partyId); pParty && pParty->GameplayOverridden)
        {
            settings.Difficulty = pParty->GameplaySettings.Difficulty;
            settings.PvpEnabled = pParty->GameplaySettings.PvpEnabled;
        }
    }
    return settings;
}

void PartyService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    const auto cCurrentTick = GameServer::Get()->GetTick();
    if (m_nextInvitationExpire > cCurrentTick)
        return;

    // Only expire once every 10 seconds
    m_nextInvitationExpire = cCurrentTick + 10000;

    auto view = m_world.view<PartyComponent>();
    for (auto entity : view)
    {
        auto& partyComponent = view.get<PartyComponent>(entity);
        auto itor = std::begin(partyComponent.Invitations);
        while (itor != std::end(partyComponent.Invitations))
        {
            if (itor->second < cCurrentTick)
            {
                itor = partyComponent.Invitations.erase(itor);
            }
            else
            {
                ++itor;
            }
        }
    }
}

void PartyService::OnPartyCreate(const PacketEvent<PartyCreateRequest>& acPacket) noexcept
{
    Player* const player = acPacket.pPlayer;
    auto& inviterPartyComponent = player->GetParty();

    spdlog::debug("[PartyService]: Received request to create party");

    if (!inviterPartyComponent.JoinedPartyId) // Ensure not in party
    {
        uint32_t partyId = m_nextId++;
        Party& party = m_parties[partyId];
        party.GameplaySettings = GetSettings();
        party.Members.push_back(player);
        party.LeaderPlayerId = player->GetId();
        inviterPartyComponent.JoinedPartyId = partyId;

        spdlog::debug("[PartyService]: Created party for {}", player->GetId());
        SendPartyJoinedEvent(party, player);

        if (m_parties.size() == 1 && GameServer::Get()->AllowsAutoPartyJoin())
        {
            for (Player* otherPlayer : m_world.GetPlayerManager())
            {
                if (otherPlayer->GetId() != player->GetId())
                {
                    party.Members.push_back(otherPlayer);
                    otherPlayer->GetParty().JoinedPartyId = partyId;

                    SendPartyJoinedEvent(party, otherPlayer);
                }
            }

            BroadcastPartyInfo(partyId);
        }
    }
}

void PartyService::OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;
    Player* const player = acPacket.pPlayer;
    Player* const pNewLeader = m_world.GetPlayerManager().GetById(message.PartyMemberPlayerId);

    spdlog::debug("[PartyService]: Received request to change party leader to {}", message.PartyMemberPlayerId);

    if (!pNewLeader)
    {
        spdlog::error("[PartyService]: Player {} does not exist. Cannot change party leader", message.PartyMemberPlayerId);
        return;
    }

    auto& inviterPartyComponent = player->GetParty();
    if (inviterPartyComponent.JoinedPartyId) // Ensure not in party
    {
        Party& party = m_parties[*inviterPartyComponent.JoinedPartyId];
        if (party.LeaderPlayerId == player->GetId())
        {
            for (auto& pPlayer : party.Members)
            {
                if (pPlayer->GetId() == pNewLeader->GetId())
                {
                    party.LeaderPlayerId = pPlayer->GetId();
                    spdlog::debug("[PartyService]: Changed party leader to {}, updating party members.", party.LeaderPlayerId);
                    BroadcastPartyInfo(*inviterPartyComponent.JoinedPartyId);
                    break;
                }
            }
        }
    }
}

void PartyService::OnPartyKick(const PacketEvent<PartyKickRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;
    Player* const player = acPacket.pPlayer;
    Player* const pKick = m_world.GetPlayerManager().GetById(message.PartyMemberPlayerId);

    spdlog::debug("[PartyService]: Received request to change party leader to {}", message.PartyMemberPlayerId);

    if (!pKick)
    {
        spdlog::error("[PartyService]: Player {} does not exist. Cannot kick", message.PartyMemberPlayerId);
        return;
    }

    auto& inviterPartyComponent = player->GetParty();
    if (inviterPartyComponent.JoinedPartyId) // Ensure not in party
    {
        Party& party = m_parties[*inviterPartyComponent.JoinedPartyId];
        if (party.LeaderPlayerId == player->GetId())
        {
            spdlog::debug("[PartyService]: Kicking player {} from party", pKick->GetId());
            RemovePlayerFromParty(pKick);
            BroadcastPlayerList(pKick);
        }
    }
}

void PartyService::OnLeaderControl(const PacketEvent<LeaderControlRequest>& acPacket) noexcept
{
    Player* const pPlayer = acPacket.pPlayer;
    if (!pPlayer || !IsPlayerLeader(pPlayer) || !pPlayer->GetParty().JoinedPartyId)
        return;
    const auto it = m_parties.find(*pPlayer->GetParty().JoinedPartyId);
    if (it == m_parties.end())
        return;
    NotifyLeaderControl notify{};
    notify.FreeControl = acPacket.Packet.FreeControl;
    for (auto* pMember : it->second.Members)
    {
        if (pMember != pPlayer)
            pMember->Send(notify);
    }
    spdlog::info("[PartyService]: leader {} free control", notify.FreeControl ? "has" : "does not have");
}

void PartyService::OnPartyReady(const PacketEvent<PartyReadyRequest>& acPacket) noexcept
{
    auto* const pPlayer = acPacket.pPlayer;
    auto* const pParty = GetPlayerParty(pPlayer);
    if (!pParty || pParty->SessionState > 2)
        return;

    // Before launch this is lobby readiness. During the launch transition the
    // same acknowledgement means that Skyrim has emitted TESLoadGameEvent and
    // the client is holding its world at the synchronization barrier.
    auto& ready = pParty->SessionState == 0 ? pParty->ReadyPlayerIds :
        pParty->SessionState == 1 ? pParty->LoadedPlayerIds : pParty->GameplayReadyPlayerIds;
    const auto found = std::find(ready.begin(), ready.end(), pPlayer->GetId());
    if (acPacket.Packet.Ready && found == ready.end())
        ready.push_back(pPlayer->GetId());
    else if (!acPacket.Packet.Ready && found != ready.end())
        ready.erase(found);
    if (pParty->SessionState == 1 && pParty->LoadedPlayerIds.size() == pParty->Members.size() &&
        std::all_of(pParty->Members.begin(), pParty->Members.end(), [&](const Player* apMember) {
            return std::find(pParty->LoadedPlayerIds.begin(), pParty->LoadedPlayerIds.end(), apMember->GetId()) != pParty->LoadedPlayerIds.end();
        }))
    {
        pParty->SessionState = 2;
        spdlog::info("[PartyService]: Every party member reached the world-ready barrier for epoch {}", pParty->StartEpoch);
    }
    else if (pParty->SessionState == 2 && pParty->GameplayReadyPlayerIds.size() == pParty->Members.size() &&
        std::all_of(pParty->Members.begin(), pParty->Members.end(), [&](const Player* apMember) {
            return std::find(pParty->GameplayReadyPlayerIds.begin(), pParty->GameplayReadyPlayerIds.end(), apMember->GetId()) != pParty->GameplayReadyPlayerIds.end();
        }))
    {
        pParty->SessionState = 3;
        spdlog::info("[PartyService]: Every party member reached the gameplay barrier for epoch {}", pParty->StartEpoch);
    }
    BroadcastPartyInfo(*pPlayer->GetParty().JoinedPartyId);
}

void PartyService::OnCheckpointSave(const PacketEvent<CheckpointSaveRequest>& acPacket) noexcept
{
    // Only the leader's saves define a checkpoint, and only inside a running shared session.
    auto* const pPlayer = acPacket.pPlayer;
    auto* const pParty = GetPlayerParty(pPlayer);
    const auto& id = acPacket.Packet.CheckpointId;
    if (!pParty || pParty->LeaderPlayerId != pPlayer->GetId() || pParty->SessionState < 2 || id.empty() ||
        id.size() > 64 || !std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }))
    {
        spdlog::warn("[PartyService]: Rejected checkpoint save request from player {}", pPlayer->GetId());
        return;
    }

    NotifyCheckpointSave notify{};
    notify.CheckpointId = id;
    notify.AuthorityEpoch = pParty->StartEpoch;
    for (auto* pMember : pParty->Members)
        pMember->Send(notify);
    spdlog::info("[PartyService]: Checkpoint {} for party of {} (epoch {})", id, pParty->Members.size(), pParty->StartEpoch);
}

void PartyService::OnPartyStart(const PacketEvent<PartyStartRequest>& acPacket) noexcept
{
    auto* const pPlayer = acPacket.pPlayer;
    auto* const pParty = GetPlayerParty(pPlayer);
    if (!pParty || pParty->LeaderPlayerId != pPlayer->GetId() || pParty->SessionState != 0)
        return;

    const auto& request = acPacket.Packet;
    if (request.Mode != PartyStartRequest::kNew && request.Mode != PartyStartRequest::kContinue)
        return;

    pParty->CampaignMode = request.Mode;
    pParty->CheckpointId = request.Mode == PartyStartRequest::kContinue ? request.CheckpointId : String{};
    if (request.Launch)
    {
        const bool allReady = pParty->Members.size() >= 2 && pParty->ReadyPlayerIds.size() == pParty->Members.size() &&
                              std::all_of(pParty->Members.begin(), pParty->Members.end(), [&](const Player* apMember) {
                                  return std::find(pParty->ReadyPlayerIds.begin(), pParty->ReadyPlayerIds.end(), apMember->GetId()) != pParty->ReadyPlayerIds.end();
                              });
        if (!allReady)
        {
            spdlog::warn("[PartyService]: Leader {} attempted to start before every member was ready", pPlayer->GetId());
            BroadcastPartyInfo(*pPlayer->GetParty().JoinedPartyId);
            return;
        }
        pParty->SessionState = 1;
        pParty->StartEpoch = m_nextStartEpoch++;
        pParty->LoadedPlayerIds.clear();
        pParty->GameplayReadyPlayerIds.clear();
        pParty->ReadyPlayerIds.clear();
    }
    BroadcastPartyInfo(*pPlayer->GetParty().JoinedPartyId);
}

void PartyService::OnPartySessionSettings(const PacketEvent<PartySessionSettingsRequest>& acPacket) noexcept
{
    auto* const pPlayer = acPacket.pPlayer;
    auto* const pParty = GetPlayerParty(pPlayer);
    if (!pParty || pParty->LeaderPlayerId != pPlayer->GetId())
        return;

    const auto& request = acPacket.Packet;
    if (request.Password.size() > 64)
        return;

    pParty->LobbyOpen = request.Open;
    pParty->PasswordProtected = request.Open && !request.Password.empty();
    GameServer::Get()->SetSessionPassword(pParty->PasswordProtected ? request.Password : String{});
    BroadcastPartyInfo(*pPlayer->GetParty().JoinedPartyId);
}

void PartyService::OnPartyGameplaySettings(const PacketEvent<PartyGameplaySettingsRequest>& acPacket) noexcept
{
    auto* const pPlayer = acPacket.pPlayer;
    auto* const pParty = GetPlayerParty(pPlayer);
    if (!pParty || pParty->LeaderPlayerId != pPlayer->GetId() ||
        pParty->SessionState == 1 || pParty->SessionState == 2 ||
        acPacket.Packet.Difficulty > 5)
        return;

    pParty->GameplaySettings.Difficulty = acPacket.Packet.Difficulty;
    pParty->GameplaySettings.PvpEnabled = acPacket.Packet.PvpEnabled;
    pParty->GameplayOverridden = true;

    NotifySettingsChange notify{};
    for (auto* pMember : pParty->Members)
    {
        notify.Settings = GetSettingsForPlayer(pMember);
        pMember->Send(notify);
    }
}

void PartyService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    BroadcastPlayerList();

    NotifyPlayerJoined notify{};
    notify.PlayerId = acEvent.pPlayer->GetId();
    notify.Username = acEvent.pPlayer->GetUsername();

    notify.WorldSpaceId = acEvent.WorldSpaceId;
    notify.CellId = acEvent.CellId;

    notify.Level = acEvent.pPlayer->GetLevel();

    spdlog::debug("[Party] New notify player {:x} {}", notify.PlayerId, notify.Username.c_str());

    GameServer::Get()->SendToPlayers(notify, acEvent.pPlayer);

    if (m_parties.size() == 1 && GameServer::Get()->AllowsAutoPartyJoin())
    {
        for (Player* player : m_world.GetPlayerManager())
        {
            if (IsPlayerInParty(player))
            {
                auto& playerPartyComponent = player->GetParty();
                Party& party = m_parties[*playerPartyComponent.JoinedPartyId];

                party.Members.push_back(acEvent.pPlayer);
                party.ReadyPlayerIds.clear();
                party.LoadedPlayerIds.clear();
                party.GameplayReadyPlayerIds.clear();
                party.SessionState = 0;
                party.StartEpoch = 0;
                acEvent.pPlayer->GetParty().JoinedPartyId = *playerPartyComponent.JoinedPartyId;

                SendPartyJoinedEvent(party, acEvent.pPlayer);

                BroadcastPartyInfo(*playerPartyComponent.JoinedPartyId);

                break;
            }
        }
        
    }
}

void PartyService::OnPartyInvite(const PacketEvent<PartyInviteRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;

    // Make sure the player we invite exists
    Player* const pInvitee = m_world.GetPlayerManager().GetById(message.PlayerId);
    Player* const pInviter = acPacket.pPlayer;

    // If both players are available and they are different
    if (pInvitee && pInvitee != pInviter)
    {
        auto& inviterPartyComponent = pInviter->GetParty();
        auto& inviteePartyComponent = pInvitee->GetParty();

        spdlog::debug("[PartyService]: Got party invite from {}", pInviter->GetId());

        if (!inviterPartyComponent.JoinedPartyId)
        {
            spdlog::debug("[PartyService]: Inviter not in party, cancelling invite.");
            return;
        }
        else if (inviteePartyComponent.JoinedPartyId)
        {
            spdlog::debug("[PartyService]: Invitee in party already, cancelling invite.");
            return;
        }

        auto& party = m_parties[*inviterPartyComponent.JoinedPartyId];
        if (party.LeaderPlayerId != pInviter->GetId())
        {
            spdlog::debug("[PartyService]: Inviter not party leader, cancelling invite.");
            return;
        }

        // Expire in 60 seconds
        const auto cExpiryTick = GameServer::Get()->GetTick() + 60000;
        inviteePartyComponent.Invitations[pInviter] = cExpiryTick;

        NotifyPartyInvite notification;
        notification.InviterId = pInviter->GetId();
        notification.ExpiryTick = cExpiryTick;

        spdlog::debug("[PartyService]: Sending party invite to {}", pInvitee->GetId());
        pInvitee->Send(notification);
    }
}

void PartyService::OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;

    Player* const pInviter = m_world.GetPlayerManager().GetById(message.InviterId);
    Player* pSelf = acPacket.pPlayer;

    spdlog::debug("[PartyService]: Got party accept request from {}", pSelf->GetId());

    // If both players are available and they are different
    if (pInviter && pInviter != pSelf)
    {
        auto& inviterPartyComponent = pInviter->GetParty();
        auto& selfPartyComponent = pSelf->GetParty();

        // Check if we have this invitation so people don't invite themselves
        if (selfPartyComponent.Invitations.count(pInviter) == 0)
            return;

        spdlog::debug("[PartyService]: Invite found, processing.");
        if (!inviterPartyComponent.JoinedPartyId) // Ensure inviter is in a party otherwise break
        {
            spdlog::debug("[PartyService]: Inviter not in party. Cancelling.");
            return;
        }

        auto partyId = *inviterPartyComponent.JoinedPartyId;
        Party& party = m_parties[partyId];

        if (party.LeaderPlayerId != pInviter->GetId())
        {
            spdlog::debug("[PartyService]: Inviter is not party leader. Cancelling.");
            return;
        }

        if (selfPartyComponent.JoinedPartyId) // Remove from party if in one already. TODO: Decide if player needs to be out of party first
        {
            spdlog::debug("[PartyService]: Invitee already in party, cancelling.");
            // RemovePlayerFromParty(pSelf, false); // skip sending left event, will override with SendPartyJoinedEvent
            return;
        }

        party.Members.push_back(pSelf);
        party.ReadyPlayerIds.clear();
        party.LoadedPlayerIds.clear();
        party.GameplayReadyPlayerIds.clear();
        party.SessionState = 0;
        party.StartEpoch = 0;
        selfPartyComponent.JoinedPartyId = partyId;

        spdlog::debug("[PartyService]: Added invitee to party, sending events");
        SendPartyJoinedEvent(party, pSelf);
        BroadcastPartyInfo(partyId);
    }
}

void PartyService::OnPartyLeave(const PacketEvent<PartyLeaveRequest>& acPacket) noexcept
{
    RemovePlayerFromParty(acPacket.pPlayer);
}

void PartyService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    RemovePlayerFromParty(acEvent.pPlayer);
    BroadcastPlayerList(acEvent.pPlayer);
}

void PartyService::RemovePlayerFromParty(Player* apPlayer) noexcept
{
    auto* pPartyComponent = &apPlayer->GetParty();
    spdlog::debug("[PartyService]: Removing player from party.");

    if (pPartyComponent->JoinedPartyId)
    {
        auto id = *pPartyComponent->JoinedPartyId;

        Party& party = m_parties[id];
        auto& members = party.Members;

        members.erase(std::find(std::begin(members), std::end(members), apPlayer));
        party.ReadyPlayerIds.clear();
        party.LoadedPlayerIds.clear();
        party.GameplayReadyPlayerIds.clear();
        party.SessionState = 0;
        party.StartEpoch = 0;

        if (members.empty())
        {
            m_parties.erase(id);
        }
        else
        {
            if (party.LeaderPlayerId == apPlayer->GetId())
            {
                party.LeaderPlayerId = members.at(0)->GetId(); // Reassign party leader
                spdlog::debug("[PartyService]: Leader left, reassigned party leader to {}", party.LeaderPlayerId);
            }
            spdlog::debug("[PartyService]: Updating other party players of removal.");
            BroadcastPartyInfo(id);
        }

        pPartyComponent->JoinedPartyId.reset();

        NotifySettingsChange defaults{};
        defaults.Settings = GetSettings();
        apPlayer->Send(defaults);

        spdlog::debug("[PartyService]: Sending party left event to player.");
        NotifyPartyLeft leftMessage;
        apPlayer->Send(leftMessage);
    }
}

void PartyService::BroadcastPlayerList(Player* apPlayer) const noexcept
{
    auto pIgnoredPlayer = apPlayer;
    for (auto pSelf : m_world.GetPlayerManager())
    {
        if (pIgnoredPlayer == pSelf)
            continue;

        NotifyPlayerList playerList;
        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pSelf == pPlayer)
                continue;

            if (pIgnoredPlayer == pPlayer)
                continue;

            playerList.Players[pPlayer->GetId()] = pPlayer->GetUsername();
        }

        pSelf->Send(playerList);
    }
}

void PartyService::BroadcastPartyInfo(uint32_t aPartyId) const noexcept
{
    auto itor = m_parties.find(aPartyId);
    if (itor == std::end(m_parties))
        return;

    auto& party = itor->second;
    auto& members = party.Members;

    NotifyPartyInfo message;
    message.LeaderPlayerId = party.LeaderPlayerId;
    message.ReadyPlayerIds = party.ReadyPlayerIds;
    message.CampaignMode = party.CampaignMode;
    message.SessionState = party.SessionState;
    message.StartEpoch = party.StartEpoch;
    message.CheckpointId = party.CheckpointId;
    message.LobbyOpen = party.LobbyOpen;
    message.PasswordProtected = party.PasswordProtected;

    for (auto pPlayer : members)
    {
        message.PlayerIds.push_back(pPlayer->GetId());
    }

    for (auto pPlayer : members)
    {
        message.IsLeader = pPlayer->GetId() == party.LeaderPlayerId;
        pPlayer->Send(message);
    }
}

void PartyService::SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept
{
    NotifyPartyJoined joinedMessage;
    joinedMessage.LeaderPlayerId = aParty.LeaderPlayerId;
    joinedMessage.IsLeader = aParty.LeaderPlayerId == aPlayer->GetId();
    for (auto pPlayer : aParty.Members)
    {
        joinedMessage.PlayerIds.push_back(pPlayer->GetId());
    }
    spdlog::debug("[PartyService]: Sending party join event to player");
    aPlayer->Send(joinedMessage);

    NotifySettingsChange settings{};
    settings.Settings = GetSettingsForPlayer(aPlayer);
    aPlayer->Send(settings);
}
