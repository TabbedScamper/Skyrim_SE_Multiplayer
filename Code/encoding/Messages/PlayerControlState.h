#pragma once

#include <TiltedCore/Buffer.hpp>

// Persistent party-leader policy, separate from the one-time gather and camera release.
struct PlayerControlState
{
    // Channels written by the Game control natives, plus POV switch. Never copy console,
    // menu input contexts, death/bleedout stored controls, or another player's UI block.
    static constexpr uint32_t kChannels = 0x5E3;
    uint64_t Epoch{};
    uint64_t Sequence{};
    uint32_t Controls{};
    uint8_t Handlers{}; // movement, look, POV
    bool Free{};
    bool Restrained{};
    bool PovScript{};

    bool IsValid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool operator==(const PlayerControlState&) const noexcept = default;
};
