#pragma once

#include <Messages/Message.h>
#include <Structs/GameId.h>
#include <cmath>
#include <Messages/WorldAnimationData.h>

enum class WorldStateKind : uint8_t
{
    Disabled, Destroyed, DestructionHealth, Open, Lock, FinishedSequence, AnimationEvent, AnimationSnapshot, Count
};

// Durable properties and ordered animation inputs. Sequence is per reference.
// FinishedSequence names a COMPLETED NiControllerSequence, never a start event.
// Cell is the containing interior cell or exterior worldspace, including its
// persistent references. IDs are always negotiated server plugin IDs.
struct WorldState
{
    uint64_t Epoch{};
    uint64_t Sequence{};
    GameId Reference{};
    GameId Cell{};
    WorldStateKind Kind{};
    uint32_t Value{};
    // Open: 0 = script live, 1 = activation cache, 2 = snapshot, 3 = baseline.
    // AnimationSnapshot: 0 = live checkpoint, 2 = restore on cell replay.
    float Scalar{};
    String Animation{};
    std::vector<uint8_t> AnimationData{};

    bool Valid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;
    bool operator==(const WorldState& aOther) const noexcept;
};
