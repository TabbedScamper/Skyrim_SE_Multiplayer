#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum class DoorVoteAction : uint8_t
{
    Vote,
    Withdraw,
    Loaded,
    Failed,
    State,
    Go,
    Cancel,
    Release,
    // Test harness: the leader moves the whole party into a cell (Destination) through the same Go / Loaded /
    // Release barrier as a door. Door == Destination marks such a vote.
    TestCell
};

struct DoorVoteData
{
    void SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void DeserializeData(TiltedPhoques::Buffer::Reader& aReader);
    bool operator==(const DoorVoteData& aOther) const noexcept;

    DoorVoteAction Action{DoorVoteAction::Vote};
    uint64_t Epoch{};
    uint64_t VoteId{};
    uint64_t Tick{};
    GameId Door{};
    GameId Cell{};
    GameId WorldSpace{};
    GameId Destination{};
    Vector3_NetQuantize Position{};
    uint32_t ReadyCount{};
    uint32_t TotalCount{};
    bool Ready{};
    TiltedPhoques::String Name{};
    TiltedPhoques::String Notice{};
};
