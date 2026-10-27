#pragma once

#include "Message.h"

using TiltedPhoques::String;
using TiltedPhoques::Vector;

struct NotifyPartyInfo final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyInfo;

    NotifyPartyInfo()
        : ServerMessage(Opcode)
        , IsLeader(false)
    {
    }

    virtual ~NotifyPartyInfo() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPartyInfo& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && IsLeader == acRhs.IsLeader && PlayerIds == acRhs.PlayerIds && ReadyPlayerIds == acRhs.ReadyPlayerIds &&
               LeaderPlayerId == acRhs.LeaderPlayerId && CampaignMode == acRhs.CampaignMode && SessionState == acRhs.SessionState &&
               StartEpoch == acRhs.StartEpoch && LeaderGeneration == acRhs.LeaderGeneration && CheckpointId == acRhs.CheckpointId && LobbyOpen == acRhs.LobbyOpen &&
               PasswordProtected == acRhs.PasswordProtected && SteamIds == acRhs.SteamIds &&
               HandoffTargetSteamId == acRhs.HandoffTargetSteamId && HandoffCheckpointId == acRhs.HandoffCheckpointId &&
               HandoffReady == acRhs.HandoffReady && HandoffSeeded == acRhs.HandoffSeeded;
    }

    Vector<uint32_t> PlayerIds{};
    // Same order as PlayerIds. Stable across the new server's player-ID allocation.
    Vector<uint64_t> SteamIds{};
    Vector<uint32_t> ReadyPlayerIds{};
    bool IsLeader;
    uint32_t LeaderPlayerId;
    uint8_t CampaignMode{};
    uint8_t SessionState{};
    uint64_t StartEpoch{};
    uint32_t LeaderGeneration{};
    String CheckpointId{};
    bool LobbyOpen{};
    bool PasswordProtected{};
    uint64_t HandoffTargetSteamId{};
    String HandoffCheckpointId{};
    bool HandoffReady{};
    bool HandoffSeeded{};
};
