#pragma once

#include <cstdint>

struct World;
struct NiPoint3;

// Character creation together (New Game in a party).
//
// Everyone creates at the same spot. While the party is still creating, a player who clicks Done
// stays in the creator: its close (UIMessageQueue::AddMessage "RaceSex Menu" hide, ID 13631) is
// held, and released on every PC together once the server says all players are done, so every
// intro continues at once. In the creator, [ and ] cycle whose character stands on the spot (this
// player's own first, then each other player's, live as they edit); the others are hidden.
namespace CreatorTogether
{
// Per frame, from PartyService. aHolding: New Game, in a party, still creating (session state 2).
void Update(World& aWorld, bool aHolding, bool aCreatorOpen) noexcept;
// This player clicked Done while the party was still creating (its close is being held).
[[nodiscard]] bool IsDone() noexcept;
// Everyone is done: close the creator now.
void Release() noexcept;
// Another player's creator state (from its live look): true once it clicked Done.
void SetRemoteReady(uint32_t aFormId, bool aReady) noexcept;
// For InterpolationSystem: where a remote player's character is shown while the creator is open
// here (on this player's spot, with its heading). False otherwise.
[[nodiscard]] bool GetDisplay(uint32_t aFormId, NiPoint3& arPosition, float& arHeading) noexcept;
} // namespace CreatorTogether
