#pragma once
#include <Structs/GameId.h>
#include <TiltedCore/Buffer.hpp>

struct QuestItemState
{
    GameId BaseId{}, QuestId{}, ReferenceId{};
    uint32_t AliasId{};
    uint32_t Count{1};
    bool QuestObject{};
    bool Active{true};
    uint64_t Revision{};
    uint32_t QuestInstance{};
    bool SameKey(const QuestItemState& aOther) const noexcept;
    bool IsValid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool operator==(const QuestItemState&) const noexcept = default;
};
