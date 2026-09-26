#pragma once

#include "Message.h"
#include "UnstuckMove.h"
#include "PlayerControlState.h"

// A one-shot snapshot, ordered by UnstuckMove, not the control heartbeat sequence.
// No inventory, actor values, quests or scripted idle identifiers travel here.
struct PartyUnstuckState
{
    PlayerControlState Controls{};
    GameId Furniture{};
    uint32_t FurnitureMarker{};
    bool Sneaking{};
    bool WeaponDrawn{};
    bool FirstPerson{};
    bool CartMode{};
    bool HudCartMode{};
    bool AIDriven{};
    bool InputBlocked{};

    bool IsValid(const UnstuckMove& acMove) const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool operator==(const PartyUnstuckState&) const noexcept = default;
};

struct RequestPartyUnstuck final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPartyUnstuck;
    RequestPartyUnstuck() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestPartyUnstuck& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Move == acRhs.Move && State == acRhs.State; }

    UnstuckMove Move{};
    PartyUnstuckState State{};
};
