# Skyrim Seamless Co-op Roadmap

This repository tracks a Skyrim Together Reborn fork focused on a stable,
two-player, Seamless Co-op-style campaign.

The official `tiltedphoques/TiltedEvolution` development branch is retained as
the `upstream` Git remote. Project work is developed against this repository's
`main` branch and should remain easy to rebase onto upstream fixes.

## Product goals

- Treat the party leader as the authority for persistent world state.
- Keep actor deaths, inventories, placed items, quests, doors, containers, and
  other persistent state consistent for every party member.
- Make death and respawning non-destructive: a follower death must not reset a
  cell or resurrect actors on that client.
- Support a persistent two-player party with simple LAN joining and an optional
  password-protected, port-forwarded server.
- Provide coordinated campaign checkpoints and a safe follower-save update
  process without copying a live or partially written Skyrim save.
- Build diagnostics into the fork so desynchronization can be reproduced and
  fixed from evidence rather than player recollection.

## Authority model

The party leader is canonical for persistent gameplay state. The dedicated
server validates and distributes that state. Followers may predict presentation
and movement, but follower-originated persistent changes must be accepted by the
leader/server authority before becoming canonical.

This distinction is important: copying the leader's `.ess` file directly to a
follower would also copy leader-specific player state. Campaign checkpointing
will therefore require an explicit state-transfer design rather than blind file
copying while the game is running.

## Phase 1: diagnostics bridge

Build a local collector and project-scoped MCP server with read-only tools:

- `session_status`
- `recent_errors`
- `mark_bug`
- `capture_snapshot`
- `compare_players`
- `compare_cell_entities`
- `inspect_actor`
- `trace_ownership`
- `compare_quests`
- `show_respawn_timeline`
- `export_bug_bundle`

The game clients and server will emit structured JSONL events. The follower PC
will send its telemetry to a collector on the leader PC over the LAN. Diagnostic
services must not be exposed by router port forwarding.

## Phase 2: server-approved respawn

The current client death path calls `PlayerCharacter::RespawnPlayer()` locally
and then sends `PlayerRespawnRequest`. `RespawnPlayer()` uses `MoveTo`, sending an
outdoor player toward Whiterun's loaded grid or an interior player to the cell's
COC position. This may unload/re-enter a cell and reconstruct actors locally.

Replace that sequence with:

1. The dead client enters a waiting state and requests a respawn.
2. The server validates the request and selects a destination from party state.
3. Same-cell revival avoids `MoveTo` if engine behavior permits it.
4. Cross-cell revival relocates only after receiving the server response.
5. The server sends an authoritative cell snapshot after relocation.
6. The client reapplies canonical actor death, inventory, and activation state
   before returning control to the player.

## Phase 3: leader-authoritative persistence

- Reject or reconcile follower-originated quest progression that conflicts with
  the party leader.
- Persist canonical dead/alive state independently of transient actor ownership.
- Persist container and placed-item state on the server.
- Prevent actor ownership churn from deleting canonical entities.
- Reconcile state on cell entry, ownership transfer, reconnection, and respawn.

## Phase 4: campaign checkpoints

- Pause checkpoint creation until both clients acknowledge a stable state.
- Save the leader and follower characters separately.
- Store a server-side campaign manifest containing quest, world, inventory, and
  synchronization metadata.
- Validate save and mod fingerprints before resuming.
- Keep rotating, recoverable checkpoint backups.

## Initial evidence

Logs from the 2026-09-20 play session show:

- repeated ownership transfers between player 3 and player 4;
- actors removed because no eligible owner remained;
- repeated failures to retrieve actors for teleportation;
- duplicate remote actor warnings; and
- a client access-violation crash after several wildlife actors, a dragon, and
  Alduin were spawned.

These symptoms make ownership tracing and synchronized state snapshots the first
diagnostic priority.

## Near-term definition of done

The first playable milestone is complete when two players can clear an interior,
one player can die and respawn, and both clients still agree on:

- every nearby actor's identity and dead/alive state;
- actor ownership and ownership epoch;
- looted items and container contents;
- active quest and quest stage; and
- player cell and position.

