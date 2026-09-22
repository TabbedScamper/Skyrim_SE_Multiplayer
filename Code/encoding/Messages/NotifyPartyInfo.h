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
        return GetOpcode() == acRhs.GetOpcode() && PlayerIds == acRhs.PlayerIds && ReadyPlayerIds == acRhs.ReadyPlayerIds &&
               LeaderPlayerId == acRhs.LeaderPlayerId && CampaignMode == acRhs.CampaignMode && SessionState == acRhs.SessionState &&
               StartEpoch == acRhs.StartEpoch && CheckpointId == acRhs.CheckpointId && LobbyOpen == acRhs.LobbyOpen &&
               PasswordProtected == acRhs.PasswordProtected;
    }

    Vector<uint32_t> PlayerIds{};
    Vector<uint32_t> ReadyPlayerIds{};
    bool IsLeader;
    uint32_t LeaderPlayerId;
    uint8_t CampaignMode{};
    uint8_t SessionState{};
    uint64_t StartEpoch{};
    String CheckpointId{};
    bool LobbyOpen{};
    bool PasswordProtected{};
};
