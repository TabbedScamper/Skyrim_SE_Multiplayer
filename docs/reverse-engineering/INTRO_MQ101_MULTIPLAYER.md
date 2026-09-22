# MQ101 (Unbound) multiplayer design

## Decision

The Helgen opening runs as one host-authoritative cinematic. Followers do not
exist as additional physical cart passengers. Before ordinary play begins,
their clients are spectators of the host's canonical camera/world, except while
each client is inside its own isolated character creator. When the host reaches
the first verified controllable state, the server materializes the other
players near the host and releases everyone together.

This preserves the authored scene's single-player assumptions while still
letting every player create a distinct character.

## Why an extra cart actor is unsafe

The shipped MQ101 quest fragment binds only `Game.GetPlayer()` to the second
cart with `SetVehicle`. The same quest directly toggles global chargen state,
player controls, player AI driving, first-person geometry, inventory, equipment,
position, scenes, aliases, and saves. Trigger and actor scripts repeatedly test
against `Game.GetPlayer()` before setting quest stages. Adding another physical
player to this machinery would create an actor that the scene does not know how
to seat, restrain, name, execute, release, or advance.

The robust abstraction is therefore a replicated host presentation, not a
second locally simulated opening scene.

## Session state machine

```text
LobbyReady
  -> HostCinematic
  -> CharacterCreationBarrier
       -> LocalCharacterCreation[player]
       -> CharacterReady[player]
  -> HostCinematicResume
  -> HostControlGranted
  -> PartyMaterializing
  -> PartyPlayable
```

Every transition has a server-issued epoch and is idempotent. Replayed or late
messages from an older epoch cannot open RaceMenu, restore controls, spawn an
actor, or advance MQ101 again.

### `HostCinematic`

- Only the host runs MQ101, its scene packages, triggers, quest fragments, and
  `MQ101DragonAttack` mutations.
- The host sends a cinematic presentation stream: camera transform/mode,
  fade state, current scene/phase, subtitle/voice cue, and a periodic world
  snapshot for visible actors and references.
- A follower's local player actor is hidden, collisionless, AI-disabled,
  activation-disabled, and excluded from Havok, packages, detection, combat,
  dialogue, triggers, aliases, and quest conditions.
- Followers render the host camera locally. We do not network raw pixels and do
  not attach their actor to the cart.
- Pause/menu state is per player, but no follower menu may advance world time or
  the host scene.

### `CharacterCreationBarrier`

The installed `Skyrim.esm` maps MQ101 stage 75 to `Fragment_13`, whose only
operation is `Game.ShowRaceMenu()`. The coordinator intercepts that transition,
allows the host to enter the native creator, and orders every follower into an
isolated local creator. Host progression to stage 80 is deferred until the ready
barrier commits: stage 80's `Fragment_11` exits chargen, requests the vanilla
autosave, and adds race spells, so letting it run early would resume the host
world before followers are ready.

- Race, appearance, name, sex, body, and player-specific starting data belong
  to that player and are not committed as shared quest state.
- Each client sees only its own preview actor. Remote player actors remain
  invisible and collisionless.
- Closing RaceMenu produces a character descriptor and manifest hash, but does
  not automatically resume MQ101.
- The co-op overlay shows each party member as `Creating`, `Ready`,
  `Disconnected`, or `Incompatible`. A player may reopen character creation
  until marking ready.
- `Ready` is reversible until the last connected player readies. The final ready
  action atomically closes the barrier.
- If someone disconnects, the host may wait, remove that member, or abort to the
  lobby. There is no silent timeout that strands the quest half-advanced.

Followers must not execute MQ101's local `ShowRaceMenu` fragment as a quest
stage consequence. The session coordinator invokes character creation in an
isolated player transaction and suppresses reflected quest events.

### `HostCinematicResume`

- The host resumes the exact paused MQ101 scene/phase and remains the only quest
  executor.
- Followers return to the replicated host camera and remain non-materialized.
- Character descriptors are distributed so clients can preload remote faces,
  equipment, skeletons, and animation graphs without spawning them into Helgen.

### `HostControlGranted`

The first intended escape handoff is MQ101 stage 160. Its bound
`Fragment_316` unrestrains the player, enables movement/looking/activation-class
controls, leaves combat controls restricted, exits cart HUD mode, and displays
the instruction to move toward the keep. Stage 140's `Fragment_295` has already
turned off player AI driving. Stage 240 later restores every player control in
the keep, but waiting for 240 would prevent followers from participating in the
outdoor dragon escape requested here.

Materialization is keyed to the committed stage-160 transaction and verified
engine state, not a guessed delay. The live guard requires all of the following:

1. the host has left RaceMenu/chargen;
2. player AI driving is off;
3. movement and activation controls are enabled;
4. the host is not mounted to a vehicle or locked in scene furniture;
5. authoritative MQ101 stage 160 and its fragment transaction have committed;
   and
6. the host actor has a valid loaded cell and stable transform.

The stage mapping above comes from the VMAD fragment bindings in the installed
master, joined to the locally decompiled PEX. It must still be confirmed by a
live two-client probe before the hook can be called correct.

### `PartyMaterializing`

- Freeze host interaction for a short server barrier; do not freeze rendering.
- Choose collision-safe points on the same navmesh around the host, preferring
  authored party markers when a compatibility profile supplies them.
- Apply the authoritative cell/world snapshot and each player's character state
  before enabling their 3D actor.
- Spawn followers hidden and collisionless, resolve ground/navmesh placement,
  then enable visibility, collision, controls, and normal replication in that
  order.
- Increment each player's lifecycle generation so no chargen/spectator
  animation, transform, or equipment packet can affect the playable actor.
- Release all clients on one committed `PartyPlayable` epoch.

## Host-only guards required for MQ101

The compatibility layer must prevent followers from independently processing:

- `SetStage`/objective changes from player hit, combat, death, item, activation,
  and trigger events;
- scene start/stop and alias enable/disable/move operations;
- cart `SetVehicle`, furniture, restraint, execution, and AI-driven transitions;
- player inventory stripping/equipping and forced movement;
- `RequestSave`/`RequestAutoSave` calls;
- Dragon Attack line-of-sight or player-relative triggers; and
- the Hadvar/Ralof branch decision.

Hadvar versus Ralof is one campaign decision owned by the host transaction. The
result is projected to every player's save; followers do not choose separately.

## Reconnect and failure recovery

- Rejoining before `PartyPlayable` restores the current intro epoch and either
  the follower spectator state or their unfinished local creator.
- Rejoining afterward skips the cinematic and joins from the latest shared
  checkpoint plus journal tail.
- Host loss during the authored cinematic aborts to the lobby for the first
  implementation. Migrating MQ101 mid-scene is explicitly deferred because its
  latent Papyrus, scene phase, aliases, furniture, and vehicle state are not
  safely reconstructible yet.
- A crash after all players ready but before materialization resumes from the
  last committed intro checkpoint; it never guesses by firing several stages.

## Acceptance tests

1. Two fresh characters can join in the main menu and see the same cart ride.
2. Both RaceMenus can be open concurrently without either client advancing
   MQ101 or exposing/colliding with another player actor.
3. One ready player can wait while the other edits and reopens their character.
4. Exactly one Hadvar/Ralof branch and one set of MQ101 mutations commits.
5. No follower trigger, hit, death, item, or activation event changes MQ101.
6. Both players materialize only after the verified host-control boundary, on
   valid nearby ground, with correct appearances and independent inventories.
7. Packet duplication/reordering cannot reopen RaceMenu or spawn a player twice.
8. A follower reconnect at every state reconstructs that state without replaying
   the quest.
9. Host loss fails visibly to the lobby without corrupting either character or
   the shared campaign.

## Installed record inventory

The installed MQ101 QUST record is a run-once, priority-80 main quest with 124
stages, 7 objectives, 136 aliases, and 17 directly linked SCEN records. The
largest opening scenes are not small cutscenes: `MQ101Scene4` alone has 55
phases, 23 actors, and 251 actions. This reinforces the host-only scene decision
and gives the runtime harness exact scene/phase checkpoints to observe.
