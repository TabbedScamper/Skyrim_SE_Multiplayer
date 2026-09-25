#pragma once

#include <Structs/GameId.h>

// Canonical network identity for one loading-screen presentation. The epoch is
// monotonic within a party session and prevents a late packet from an earlier
// load from being applied to a later LoadingMenu instance.
struct LoadingScreenPresentation
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    bool operator==(const LoadingScreenPresentation& acRhs) const noexcept { return Epoch == acRhs.Epoch && LoadScreenId == acRhs.LoadScreenId; }

    uint64_t Epoch{};
    GameId LoadScreenId{};
};
