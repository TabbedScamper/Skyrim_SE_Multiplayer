# Shared campaign persistence

## Implemented foundation

The dedicated/listen server owns a SQLite campaign database. Its metadata is:

- stable campaign UUID;
- monotonically increasing committed revision;
- authority epoch for rejecting mutations from an obsolete leader;
- latest complete checkpoint revision.

Every shared mutation receives a unique transaction ID. Committing the same ID
and payload again returns the original revision; reusing the ID for different
data is rejected. Journal insert and revision advancement happen in one
`BEGIN IMMEDIATE` transaction with WAL and full synchronization enabled.

Leader quest changes are the first live domain on this path. The client sends a
transaction ID, the server verifies leader authority, durably commits the quest
payload, updates its in-memory quest log, and broadcasts the committed revision
and authority epoch. Disabled miscellaneous quests and malformed statuses are
rejected before persistence.

On authentication, every client receives the stable campaign UUID, current
revision, and authority epoch. This is metadata only; joining clients do not yet
receive or apply a complete world snapshot.

## Safety boundary

No native `.ess` or `.skse` save is written or copied by this layer. Raw Skyrim
saves contain player-specific state and Papyrus/runtime state, so copying the
leader's file would overwrite the follower and can preserve incompatible VM
handles. The eventual save flow will be a barrier:

1. stop accepting new shared mutations;
2. drain and commit the campaign journal;
3. create a server checkpoint at revision `N`;
4. make each client apply snapshot/tail through `N`;
5. make each client create its own Skyrim save and co-save projection tagged
   with campaign ID, player ID, revision `N`, protocol version, and mod hash;
6. resume only after acknowledgements or report which player failed.

## Next slices

1. Campaign snapshot request/response and journal-tail reconnect convergence.
2. Save barrier prepare/ready/commit/ack protocol, initially exercised entirely
   by protocol bots without calling Skyrim's save manager.
3. Server-authored player lifecycle transactions replacing local client-first
   respawn: alive, downed, reviving, dead, spectating, respawning, alive.
4. SKSE co-save metadata and safe per-player projection after the barrier has
   passed loss/reorder/reconnect tests.
5. Persisted reference, corpse, container, item-instance, and encounter-reset
   projections under the same revision stream.
