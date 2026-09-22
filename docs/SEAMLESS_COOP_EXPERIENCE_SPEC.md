# Skyrim SE Multiplayer — seamless co-op experience specification

## Product promise

Two players can form a private Steam party from Skyrim's main menu, begin or
continue one shared campaign, and remain together through doors, dialogue,
quests, death, saving, and reconnecting. The party leader is the canonical
world authority. A follower may propose actions, but a follower never commits
world state independently.

This is the acceptance target, not a description of current Skyrim Together
Reborn behavior.

## Non-negotiable invariants

1. One persistent reference has one session identity, lifecycle generation,
   owner, and canonical state.
2. Only the leader-authoritative simulation may commit NPC AI, quest, scene,
   container, dropped-item, door, encounter, clock, or reset mutations.
3. Every committed mutation has a monotonically increasing sequence number and
   an idempotency key. Replaying an event cannot apply it twice.
4. A joining or recovering client applies a complete snapshot before receiving
   live events. Events older than the snapshot watermark are discarded.
5. Player death never reloads an old world. It changes only that player's
   lifecycle state unless the party explicitly chooses a full-party reset.
6. Saving is a barrier: drain authoritative transactions, persist the shared
   snapshot/journal watermark, then materialize each player's save with shared
   world state plus that player's character state.
7. If exact synchronization cannot be guaranteed, interaction is temporarily
   blocked with visible feedback; the game must not silently allow two
   contradictory outcomes.

## Main-menu party and campaign flow

1. `Co-op` is a first-class main-menu entry.
2. `Host` creates a password-protected Steam lobby; `Join Friend` uses the Steam
   friends/lobby UI. LAN peers still use Steam relay unless direct LAN is
   explicitly selected.
3. The lobby displays both players, readiness, game/runtime version, mod/plugin
   manifest hash, and campaign compatibility before Skyrim loads a save.
4. The host chooses `New Shared Campaign` or `Continue Shared Campaign`.
5. For a new campaign, the leader alone executes the opening quest and world
   simulation. Followers spectate the leader's canonical cinematic camera with
   their player actors absent from physics, AI, triggers, dialogue, and aliases.
   At the authored character-creation boundary, the host scene pauses and every
   player opens an isolated local creator. A visible ready barrier holds the
   scene until all connected players commit their character. Everyone then
   returns to the host camera; when the host reaches a verified controllable
   state, followers materialize at collision-safe positions around the host and
   all clients enter play on one server epoch. See
   `docs/reverse-engineering/INTRO_MQ101_MULTIPLAYER.md`.
6. For an existing campaign, the leader selects the shared campaign checkpoint.
   Each player loads their character projection of that checkpoint. Nobody
   manually connects, teleports, or creates a party after loading.
7. The session persists through individual death, loading screens, and ordinary
   quest completion. Leaving is an explicit `Leave Session` action.

## Authority and presentation rules

| Domain | Canonical authority | Follower behavior | Required recovery |
|---|---|---|---|
| NPC AI/packages/targets | Party leader | Render a network proxy; do not independently select packages or targets | NPC state snapshot and lifecycle rebuild |
| Player movement/combat intent | That player, validated by session authority | Other clients interpolate the canonical stream | Transform/animation keyframe |
| Dialogue and scenes | Exclusive server-issued conversation lease; world consequences commit through leader | Spectate the same speaker, subtitle, line, choices, and selected response | Conversation snapshot or clean abort |
| Quests/aliases/globals | Leader transaction journal | Mirror committed state; interaction becomes a proposal | Quest transaction snapshot |
| Doors/load transitions | Leader-coordinated transition barrier | Queue, show status, then transition from the same source to destination | Transition epoch and destination snapshot |
| Containers/pickups/drops | Leader-authoritative atomic inventory transaction | Predict presentation only; reconcile to commit | Inventory/container snapshot |
| Loose-object physics | Leader while awake; canonical settled transform afterward | Kinematic/interpolated proxy | Latest transform and sleep state |
| Death/ragdoll/corpses | Leader simulation for NPCs; each player owns their death request | Display streamed pose, then freeze the canonical final corpse pose | Actor lifecycle generation plus corpse pose |
| Time/weather/cell resets | Leader session clock | Render received state; never advance reset clocks independently | Clock, weather and cell-reset snapshot |
| Save/checkpoint | Shared session coordinator | Persist player-specific projection at the same watermark | Last committed checkpoint plus journal tail |

## Interaction contracts

### NPC dialogue

- Activating an NPC requests a lease for its persistent reference.
- Once granted, all other activations are rejected while the lease is live.
  The other player sees `NPC is speaking with <player>` rather than opening a
  second dialogue menu.
- Both players see the same subtitles, current topic, available player choices,
  highlighted choice, committed choice, and NPC response.
- Only the lease holder controls choices. A later optional mode may allow the
  party to vote, but voting must not execute multiple dialogue branches.
- Scene phase, participants, aliases, AI packages, and quest effects commit as
  one ordered conversation transaction. Disconnect or timeout releases the
  lease and either resumes from a known phase or aborts the scene cleanly.

### Doors and cell transitions

- Activation creates a transition intent containing door identity, source
  cell, destination cell/marker, and transition epoch.
- Only one party transition may commit at a time. A second request is queued
  visibly rather than being lost or run concurrently.
- The leader loads the destination and publishes its initial cell snapshot.
  Followers transition only after acknowledging readiness, then apply that
  snapshot before controls are restored.
- Followers may choose `Follow` or `Stay` for non-quest doors. Quest-critical
  transitions may require the party together and explain why.

### Dropped items and movable objects

- Dropping is one atomic transaction: remove the inventory instance, allocate
  a session object ID, create its reference, and broadcast its initial state.
- Item instance data includes base form, stack count, enchantment, temper/health,
  charge, poison, ownership/stolen state, custom name, and other relevant
  ExtraData. Base form plus count is not a sufficient identity.
- The leader simulates loose physics. Followers interpolate the object and do
  not feed their collision result back into authority.
- On sleep, the final transform becomes persistent. Pickup consumes the same
  session object ID exactly once and atomically moves the item into inventory.

### Animation, death, and ragdolls

- Every actor stream carries a spawn/lifecycle generation. Animation or death
  messages from an older generation are rejected.
- Network state distinguishes alive, bleedout, dying, dead/ragdoll, corpse,
  resurrecting, and alive-after-respawn. A remote player can never retain the
  visual graph from a prior dead generation.
- Animation replication sends important events plus periodic behavior-state
  keyframes/checksums. A mismatch triggers a controlled graph rebuild from the
  latest keyframe instead of continuing corrupted state.
- NPC ragdolls are simulated by the leader. During motion, followers receive a
  root transform and a compact set of important bone poses. When settled, the
  canonical final pose is frozen and included in join/reconnect snapshots.
- If full bone streaming proves too unstable, the fallback is a canonical death
  animation and final root pose—not two independently simulated ragdolls.

### Player downed, revival, death, and respawn

Player defeat is a server-authoritative state machine:

`Alive -> Downed -> Reviving -> Alive`

or:

`Alive/Downed -> Dead -> Spectating -> Respawning -> Alive`

- Lethal damage is proposed to the session authority before Skyrim commits
  ordinary player death or loads a save.
- The player becomes `Downed` when at least one eligible teammate is connected,
  alive, in the same loaded cell/worldspace, and within the configured rescue
  radius. Initial defaults are a 3,000 Skyrim-unit rescue radius and a 60-second
  bleedout timer; both remain server settings to be tuned in two-machine tests.
- A downed player uses Skyrim's bleedout presentation, stays at a small positive
  health floor, cannot attack, move normally, activate the world, open combat
  inventory, fast travel, or start dialogue, and cannot take further ordinary
  damage. They retain their complete inventory and all progression.
- A nearby living teammate receives a `Hold Activate to Revive` interaction.
  The initial revive duration is three seconds. Leaving range, releasing the
  control, taking damage, becoming downed, changing cell, or losing the lease
  cancels it. The server grants only one revive lease and commits it once.
- A successful revive plays the canonical bleedout-stop/get-up transition,
  restores an initial 30% health, increments the player's lifecycle generation,
  rebuilds the remote animation state, and grants a short configurable damage
  grace period. It never reloads a save.
- If no eligible teammate exists when lethal damage occurs, the player becomes
  `Dead` immediately. If the last eligible teammate leaves range, disconnects,
  dies, or becomes downed, a short network grace period expires and the downed
  player becomes dead. The bleedout timer also ends in death.
- Dead players spectate until the respawn rule allows them to return at the last
  shared checkpoint or another safe session spawn. Respawn restores the player
  actor only. Inventory, experience, quest state, discovered state, picked-up
  loot, containers, NPC deaths, corpse state, and the session journal are not
  rolled back.
- An individual death never resets enemies. A full-party wipe may perform one
  explicit, server-authored encounter reset if that campaign setting is enabled.
  Every client applies the same reset epoch; it is not implemented by loading an
  earlier save. Quest and inventory transactions remain committed either way.
- Killmoves, scripted instant-death effects, falls, damage-over-time, hazards,
  transformations, and cell changes must all enter through the same transition
  guard. Exceptions are data-driven and cannot silently fall back to Skyrim's
  normal reload-save path.

### Quests and shared progression

- Client quest events are proposals, never authoritative commits.
- The leader evaluates the interaction and emits one transaction containing
  stage changes, objectives, aliases, reference enable/disable/move operations,
  scene changes, quest items, and relevant globals.
- Reflected asynchronous Skyrim events are tagged with the transaction's
  suppression/idempotency context and cannot echo into a second commit.
- Random outcomes and leveled selections are chosen once and journaled.
- Known exceptional quests may use small data-driven compatibility patches, but
  the default system must not be a pile of quest-specific timing delays.

## User-reported acceptance scenarios

| ID | Scenario | Pass condition |
|---|---|---|
| SC-001 | Form party at main menu and start fresh | Both players appear ready before loading; one action begins the same shared campaign without an in-game connect/party step |
| SC-002 | NPC walks, idles, fights, or changes package | Both clients show the same canonical NPC target, package, transform, and lifecycle generation |
| SC-003 | Leader begins conversation | Follower sees speaker/subtitles/choices and cannot independently activate that NPC |
| SC-004 | Leader selects a response | Follower sees the selection and response; exactly one quest/dialogue consequence commits |
| SC-005 | Two players activate a door together | One ordered transition runs; the second request queues; both arrive from the same snapshot |
| SC-006 | Player drops a unique enchanted item | It becomes visible on both clients with identical instance data and can be picked up exactly once |
| SC-007 | Loose item is kicked across the floor | Follower converges on leader motion and final sleeping transform without persistent sliding |
| SC-008 | Enemy dies violently | Both clients end with the same dead lifecycle and acceptably matching final corpse pose |
| SC-009 | Player takes lethal damage near living teammate | Player enters synchronized bleedout; teammate sees one revive prompt; inventory and world remain unchanged |
| SC-010 | Teammate completes revive | Both clients observe one get-up transition, matching lifecycle generation and restored health; no save loads |
| SC-011 | Player takes lethal damage with no eligible teammate | Player dies and later respawns from current session state with all acquired inventory; enemies and quests do not roll back |
| SC-012 | Downed player's teammate leaves, disconnects, or is downed | After the network grace period, the downed player transitions once to dead rather than remaining stuck |
| SC-013 | Entire party is defeated | One coordinated party-wipe policy executes on all clients; no client independently reloads a save |
| SC-014 | Player animation graph desynchronizes | Checksum detects it and graph reconstruction restores the correct alive/equipment/action state |
| SC-015 | Quest stage fires asynchronously on both clients | Server commits one transaction; reflected events produce no duplicate stage, item, scene, or alias mutation |
| SC-016 | Follower reconnects after several minutes | Snapshot plus journal tail produces the leader's NPC, corpse, item, quest, door, clock, and weather state before play resumes |
| SC-017 | Party saves and exits | Both saves share one world watermark while preserving each player's character identity and inventory |
| SC-018 | Fresh party plays the Helgen opening | Only the host executes MQ101; followers spectate the same cinematic and do not create extra cart/scene actors |
| SC-019 | Party reaches character creation | Every player edits an independent character concurrently; world simulation stays paused until the visible ready barrier completes |
| SC-020 | Host reaches the escape control handoff | Followers materialize once on valid nearby ground with correct appearance/state; no local MQ101 event or duplicate packet advances/spawns twice |

## Delivery order

1. Session protocol foundations: identities, lifecycle generations, sequence
   numbers, snapshots, journal, idempotency, reconnect.
2. Player downed/revive/death vertical slice with retained inventory and no
   save reload, including disconnect and full-party-wipe tests.
3. Main-menu Steam lobby and shared-campaign bootstrap.
4. Leader-only NPC authority and actor lifecycle reconciliation.
5. Exclusive dialogue leases, shared dialogue presentation, and scene gating.
6. Door transition coordinator.
7. Atomic inventory/container/drop transactions and leader physics ownership.
8. NPC death/ragdoll final-pose replication.
9. Quest transaction capture, alias/global/scene state, and save projections.

Each slice requires an offline two-client simulation test, packet loss/reorder/
duplicate tests, reconnect snapshot tests, and a live two-machine probe before
it may be called complete.
