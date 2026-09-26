#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

// Park and resume share the ownership epoch. Release ends suppression without
// applying a transform, for example when the leader leaves the old loaded area.
enum class ScriptedActorPhase : uint8_t
{
    Park,
    Resume,
    Release,
    Bind
};

struct ScriptedActorState
{
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    GameId WorldSpaceId{};
    GameId CellId{};
    Vector3_NetQuantize Position{};
    ScriptedActorPhase Phase{ScriptedActorPhase::Park};
    bool Disabled{};

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool operator==(const ScriptedActorState& acRhs) const noexcept;
};
