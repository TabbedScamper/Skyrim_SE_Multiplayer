#pragma once

#include <cstdint>

// Identifies the party leader within a session. The server owns the generation.
struct AuthorityEpoch
{
    uint64_t StartEpoch{};
    uint32_t LeaderPlayerId{};
    uint32_t LeaderGeneration{};

    [[nodiscard]] bool operator==(const AuthorityEpoch& aOther) const noexcept = default;
};

static_assert(sizeof(AuthorityEpoch) == 16);
