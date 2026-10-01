Refute this change. Read-only: do not edit files, build, or run the game. Put your full findings in your final answer.

Repo: C:\Users\mwalt\SkyrimSeamlessCoop-ui (Skyrim SE co-op mod; client is a DLL inside the game, server is a separate process; CEF overlay UI).

Feature: drop-in join. A player joins a running co-op session mid-game with a character taken from any of their own saves. Guest model: the host's world rules; the joiner's own save is never overwritten.

Flow: the joiner picks a character (.snap file, a character snapshot written next to each save) in the title lobby. The server asks the leader to save; the leader streams that save in 16 KB chunks through the server to the joiner only; the joiner loads it solo, applies their own character snapshot (appearance, race/sex/tints, skills, perks, spells, shouts, words, items, level), steps 120 u aside, reports Loaded; the server replays world state to them and admits them; the screen fades in.

Files:
- Code\encoding\Messages\DropIn.h (messages, limits), Code\encoding\Structs\CharacterSnapshot.{h,cpp}
- Code\server\Services\DropInService.{h,cpp}
- Code\client\Services\DropInService.h, Code\client\Services\Generic\DropInService.cpp
- Code\client\Services\CharacterSnapshots.h, Code\client\Services\Generic\CharacterSnapshots.cpp
- Code\client\Services\Generic\CheckpointSaves.cpp (save queueing, QueueLoad, .snap capture after saves, CharactersJson)
- Code\client\Services\Generic\OverlayClient.cpp (joinRunningSession, listCharacters)
- UI: Code\skyrim_ui\src\app\components\title-coop-lobby\*

Claims to try to break, each with file:line evidence:
1. A malicious or broken peer or server cannot make the joiner write outside the intended save path, allocate unbounded memory, or crash (chunk offsets, totals, hash, text lengths, snapshot list lengths).
2. Only the leader's chunks reach only the joiner; a third party in the party cannot inject or receive them; a stale attempt (abort, timeout, rejoin) cannot mix chunks from two attempts.
3. The joiner's own saves are never overwritten or deleted; the received host save and the dropin_ files cannot be mistaken for the joiner's characters later (CharactersJson, checkpoint registry).
4. Applying a snapshot cannot leave the player in a broken state on failure midway (race switch, 3D reset, tints), and every failure path ends in a visible error and a usable game, not a black screen.
5. Server state: the 120 s timeout, abort, a leader disconnect mid-transfer, and a joiner disconnect mid-transfer all clean up; a second join while one is running is handled.
6. Anything that would break with 3 to 5 players instead of 2 (the design target is 5).
7. Anything on the game thread that can stall a frame noticeably (file IO, hashing, base64, large allocations).

For each claim: REFUTED (concrete failing input and line), HOLDS (lines that make it hold), or UNVERIFIED. Rank real defects by severity.
