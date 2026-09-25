#pragma once

#include <Structs/GameId.h>

// A read-only observation of one native scene transition. The phase word is
// deliberately raw until its 1.7.104 layout is validated beyond MQ101.
struct SceneTimelineSnapshot
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
    bool operator==(const SceneTimelineSnapshot&) const noexcept = default;

    uint64_t Tick{};
    uint64_t AuthorityEpoch{};
    uint64_t TransactionId{};
    uint64_t ServerSequence{};
    GameId SceneId{};
    GameId QuestId{};
    uint32_t RawPhaseWord{UINT32_MAX};
    bool Playing{};
};
