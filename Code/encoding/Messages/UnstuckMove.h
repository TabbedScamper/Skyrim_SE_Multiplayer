#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

struct UnstuckMove
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool IsValid() const noexcept;
    bool operator==(const UnstuckMove&) const noexcept = default;

    uint64_t Epoch{};
    uint64_t Sequence{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    float Heading{};
};
