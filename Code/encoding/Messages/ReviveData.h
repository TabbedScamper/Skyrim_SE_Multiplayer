#pragma once

#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

enum class ReviveAction : uint8_t
{
    State,
    Hold,
    Cancel,
    Finish,
    Grant,
    // Call back a fallen (dead) party player. Client -> server: the caster asks for PlayerId. Server -> the fallen
    // player: come back; Cell/WorldSpace/Position are the caster's.
    Raise,
    // Server -> every member: nobody in the party is standing (all down or fallen). Everyone collapses; the party
    // then reloads the leader's latest checkpoint with everyone alive.
    Wipe
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
    // Bleedout meter while down (1 = just went down, 0 = bled out); drains over 2 minutes, paused while someone
    // revives this player. The revive restores this share of health. Sent as 1/65535 steps.
    float Bleed{1.f};
    // Bled out: the player is fallen and spectates until an ally calls it back (Raise). The server keeps this per
    // party member, so reloading a save or reconnecting does not bring the player back.
    bool Dead{};
    // Fallen to a fling or an overkill hit: the body stays visible (ragdolling) until it comes to rest.
    bool Flung{};
    GameId Cell{};
    GameId WorldSpace{};
    Vector3_NetQuantize Position{};
};
