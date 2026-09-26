#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum class ReviveAction : uint8_t
{
    State,
    Hold,
    Cancel,
    Finish,
    Grant
};

struct ReviveData
{
    void SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void DeserializeData(TiltedPhoques::Buffer::Reader& aReader);
    bool operator==(const ReviveData& aOther) const noexcept;

    ReviveAction Action{ReviveAction::State};
    uint64_t Epoch{};
    uint64_t Revision{};
    uint32_t PlayerId{};
    uint32_t ReviverId{};
    bool Down{};
    bool Alive{};
    bool InCombat{};
    GameId Cell{};
    GameId WorldSpace{};
    Vector3_NetQuantize Position{};
};
