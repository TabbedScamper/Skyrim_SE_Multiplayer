#pragma once

struct World;

// Players pass through each other while the game is driving them: before the shared session is
// playable, while this player's movement controls are disabled or it is AI-driven (cinematics,
// scripted walks, character creation), and for a few seconds after. Otherwise two players stopping
// on the same marker push each other (measured: the follower was shoved when both scripted walks
// out of the Helgen cart ended together). Only the other players' character controllers change,
// so each player still collides with the world and NPCs.
namespace PlayerCollision
{
// Main thread, a few times a second.
void Update(World& aWorld) noexcept;
// Set while a follower's player mirrors the leader's scripted package.
void SetMirroringScript(bool aMirroring) noexcept;
} // namespace PlayerCollision
