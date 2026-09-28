#pragma once

#include <cstdint>
#include <mutex>

struct World;
struct NiPoint3;
struct NotifyPlayerAppearance;

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
// Every other party player in the creator has pressed Done (or none is known). Continued sessions close on this.
bool OthersDone() noexcept;
// Everyone is done: close on the next main-thread frame. Force honors a server release even
// when a local unready crossed the final ready packet, only for this shared creator session.
void Release(bool aForce = false) noexcept;
// From the main loop (Main::Update): performs a requested release, since the menu's close is not
// safe from the off-main-thread client update.
void OnMainFrame() noexcept;
// Another player's creator state (from its live look): true once it clicked Done.
void SetRemoteReady(uint32_t aFormId, bool aReady) noexcept;
// Coalesced by actor and applied on the main thread before preview visibility.
void QueueAppearance(uint32_t aFormId, const NotifyPlayerAppearance& acAppearance) noexcept;
// Serialize the client thread's FaceGen traversal with received appearance rebuilds.
std::mutex& AppearanceMutex() noexcept;
// For InterpolationSystem: where a remote player's character is shown while the creator is open
// here (on this player's spot, with its heading). False otherwise.
[[nodiscard]] bool GetDisplay(uint32_t aFormId, NiPoint3& arPosition, float& arHeading) noexcept;
} // namespace CreatorTogether
