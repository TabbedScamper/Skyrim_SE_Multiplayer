#pragma once

#include "Message.h"
#include <Structs/GameId.h>

// An animation-graph prop on an owned actor (AnimationObjects Load 43571 / Draw 43572; the graph's load/draw events
// or a save restore put it there). Copies never play the owner's idle clip, so they never load it themselves.
struct AnimObjectRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kAnimObjectRequest;

    AnimObjectRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const AnimObjectRequest& acRhs) const noexcept
    {
        return Id == acRhs.Id && Kind == acRhs.Kind && AnimObject == acRhs.AnimObject && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t Id{};
    enum : uint8_t
    {
        kLoad,
        kDraw,
        kDetach, // AnimationObjects Detach 43577: the ANIO's unload event (BNAM) ends it
        kKindCount
    };
    uint8_t Kind{};
    GameId AnimObject{};
};
