# Native authority seams — universal party-leader authoritativeskyrim

Research-only. No production code touched. Dirty worktree preserved; the
in-progress object-physics protocol (`PhysicsReferenceUpdate`,
`NotifyPhysicsReferencesMove`, `PhysicsReferencesMoveRequest` plus the
modified `ObjectService`/`CharacterService` files) is deliberately **not**
duplicated here — this document is the engine-integration contract that
protocol must sit on.

Confidence tags: **Verified (local)** = header/body read this run from the
pinned local clone. **Verified (upstream)** = fetched from primary URL this
run. **Reported** = prior-run inspection carried from
[DEEPENING_PLAN](DEEPENING_PLAN.md) §1, not re-opened. **PROBE** = must be
confirmed on runtime 1.7.104 before any code depends on it.

Version rule (from [SOURCE_CATALOG](SOURCE_CATALOG.md)): cite
`RELOCATION_ID(SE, AE)` pairs, never raw offsets. Every AE-lineage ID is a
PROBE for 1.7.104 until `versionlib-1-7-104-0.bin` or a live probe confirms
it. CommonLibSSE-NG pin for all headers below:
`b93280e832f263dbef44e44cbe2936622a02f91a`, MIT
(`AnalysisTools/repos/CommonLibSSE-NG`, LICENSE first line verified local).
Upstream `PlayerCamera.h` on `main` fetched this run is byte-identical to the
pin — no drift for that header.

## Architecture decision after paired cart trial (2026-09-22)

The project already has a separate network server, but it does not execute
Skyrim's full gameplay engine. Running that process on a third machine would
not change who simulates MQ101. Building a complete headless Skyrim simulation
would require replacing or hosting quest/Papyrus scheduling, Havok constraints,
AI packages, dialogue scenes, and animation evaluation. That is a separate
engine-reimplementation project, not a transport configuration change.

For the co-op target, use the party leader's live Skyrim process as the sole
engine authority for shared cells and scripted scenes. The separate server
remains the sequencer and persistence authority: it grants versioned leases,
rejects stale messages, relays timestamped snapshots/events, and stores shared
campaign state. Followers keep local Skyrim for rendering, input, private UI,
and player-specific simulation, but must not independently commit shared
quest/NPC/scene/physics outcomes. When the leader does not load a cell, the
server can grant an explicit temporary simulation lease to another player.

The next native boundary is scene-driven reference ownership, distinct from
passive loose-clutter physics. Capture leader reference/rigid-body and driver
state, time-stamp it, and apply it to a follower proxy at a verified
post-animation/pre-render boundary. Do not keyframe or teleport an actively
scripted activator merely because its reference position differs. Test on the
cart as an observable fixture, then generalize by reference driver/class.
Reconcile audio/subtitles from the same scene timeline; a follower's native
remote-NPC voice currently starts independently before the host-sourced
replay. Preserve native scene-completion semantics when muting local audio.

Acceptance: in a two-PC fresh campaign, leader and follower reference,
rigid-body, and camera snapshots stay within a visual error budget through
the entire intro, with no freeze, snap, double voice, or follower-leading
scene line. Repeat across runs and network jitter, then profile CPU/bandwidth
and extend to five players. The 2026-09-22 build only passed the old
cart-freeze point in one run; it did not meet this criterion.

## 1. Havok rigid bodies and constraints

Verified (local), `include/RE/B/bhkRigidBody.h`:

- Read seam: vtable slots `0x33 GetPosition`, `0x34 GetRotation`,
  `0x38 GetCenterOfMassLocal`, `0x39 GetCenterOfMassWorld`,
  `0x3A GetTransform`, `0x3B GetAabbWorldspace` (`hkVector4`/`hkTransform`
  out-params). Write seam: slots `0x35 SetPosition`, `0x36 SetRotation`,
  `0x37 SetPositionAndRotation`.
- Velocity seam: non-virtual `SetLinearVelocity`, `SetAngularVelocity`,
  `SetLinearImpulse`, `SetAngularImpulse` — declared in the header, defined
  in the `.cpp`, so each needs its own RelocationID lookup → **PROBE**.
- Constraint seam: member `unk28` at `+0x28`, `BSTArray<void*>` of smart
  pointers to constraints (comment in header). Element type and add/remove
  entry points → **PROBE**.
- World-attach seam (base `NiObject` slots, same header): `0x29
  MoveToWorld(bhkWorld*)`, `0x2A RemoveFromCurrentWorld`.

Verified (local), `include/RE/B/bhkWorld.h`:

- The native wrapper exposes the live `hkpWorld` via slots `0x27/0x28
  GetWorld1/2` (`referencedObject.ptr`).
- Thread seam: members `worldLock` at `+0xC598` and a second
  `BSReadWriteLock` at `+0xC5A0`; `sizeof(bhkWorld) == 0xC600`. Reads under
  the read lock are the only sanctioned concurrent access observed in the
  header → writes must go through the game thread outside the Havok step;
  the exact step hook (`stepFromOutside` caller) → **PROBE**.
- World-scale globals: `GetWorldScale` ID `(231896, 188105)`,
  `GetWorldScaleInverse` ID `(230692, 187407)` — needed to convert
  host snapshots into engine units.

Not in the inspected headers (PROBE, never fact):

- Canonical identity: the `TESObjectREFR → NiAVObject →
  bhkCollisionObject → bhkRigidBody` chain is the expected path, but the
  exact accessor/offset was not read this run → **PROBE** (candidate:
  `TESObjectREFR::GetNiAVObject` + collision-object extra data).
- Motion type, activation/sleep state, deactivation: no accessor in
  `bhkRigidBody.h`/`bhkEntity.h`; they live on `hkpRigidBody` behind
  `referencedObject` → **PROBE** (Havok `setMotionType`, `activate`,
  `markForWrite` equivalents need a verified call site).
- Follower stop-sim: candidate = `RemoveFromCurrentWorld` (slot `0x2A`)
  or motion-type pin to keyframed, then `SetPositionAndRotation` (slot
  `0x37`) + velocity setters per host snapshot. Both halves → **PROBE**:
  removing from the world may break contact listeners and re-add ordering.

Carts (`bhkCartTether`) only as one example: the string `CartTether`
appears in **none** of the CommonLibSSE-NG headers (verified local,
zero hits). The shipped MQ101 path binds the player with `SetVehicle`
(see [INTRO_MQ101_MULTIPLAYER](INTRO_MQ101_MULTIPLAYER.md)), so cart sync
is one consumer of the generic rigid-body/actor-attach contract, not a
separate design.

## 2. `PlayerCamera` in cinematic/animated states

Verified (local), `include/RE/P/PlayerCamera.h`:

- 13-state enum `CameraState`: `kFirstPerson=0 … kAnimated=8 … kDragon=12`,
  `kTotal=13`. All scripted cinematics funnel through state slots, with
  `kAnimated`/`kTween`/`kPCTransition` the cinematic carriers.
- `cameraStates[13]` at `+0xB8`, `tempReturnStates` at `+0x40`,
  `cameraTarget` (`ActorHandle`) at `+0x3C`, `worldFOV` at `+0x13C`,
  `firstPersonFOV` at `+0x140`, `yaw` at `+0x154`,
  `sizeof(PlayerCamera) == 0x168`. `SetCameraRoot` = vtable slot `01`
  (TESCamera override).

Verified (local), `include/RE/T/TESCamera.h` +
`include/RE/T/TESCameraState.h`:

- `currentState` at `+0x28`, `cameraRoot` (`NiPointer<NiNode>`) at `+0x20`,
  `sizeof(TESCamera) == 0x38`.
- State vtable: `01 Begin`, `02 End`, `03 Update(nextState&)`,
  `04 GetRotation`, `05 GetTranslation`, `06 SaveGame`, `07 LoadGame`,
  `08 Revert`; members `camera` at `+0x10`, `id` at `+0x18`,
  `sizeof(TESCameraState) == 0x20`.
- `TESCamera::SetState` is a plain (non-virtual) method with **no static
  RelocationID** — its address comes from generated `Offset` data →
  **PROBE** for 1.7.104 before calling it.

Host-camera follower mode (contract, universal across cinematics):

1. Leader publishes `(state id, cameraRoot world transform, target
   handle, FOV)`; transition boundaries are `Begin`/`End` (or polling
   `currentState->id` until the hook PROBE lands).
2. Follower drives its own `cameraRoot`/`NiCamera` from snapshots and
   mirrors FOV; it never writes `currentState` directly and never calls
   unverified setters — the sanctioned handoff is `Update`'s
   `nextState` return path.
3. `kAnimated` is not special-cased: any state id travels the same
   snapshot path, which is what makes this work for all scripted
   cinematics instead of one quest.

Prior art (source-inspected): SmoothCam's
[`hooks.cpp`](https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/source/hooks.cpp)
hooks vtable slot `03` on each camera state and applies its camera work after
the native update. Its
[`camera.cpp`](https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/source/camera.cpp)
writes `cameraRoot->local.translate`, `cameraRoot->world.translate`, and the
child `NiCamera->world.translate`. The multiplayer implementation extends that
boundary to the complete transform and previous-world transforms. SmoothCam is
architectural prior art only; no source was copied.

## 3. Actor animation skeleton and ragdoll

Verified (local), `include/RE/I/IAnimationGraphManagerHolder.h`:

- Event injection: slot `01 NotifyAnimationGraph(eventName)`.
- Manager access: slots `02/03 Get/SetAnimationGraphManagerImpl`
  (pure virtual — authoritative holder owns the manager pointer).
- State observation without writes: slots `10/11/12
  GetGraphVariableImpl{1,2,3}` (float/int/bool graph variables).
- `BSAnimationGraphManager` is a `BSTEventSink`; the event struct layout
  (`{tag, holder, payload}`, reportedly `0x18`) was **not** re-inspected
  this run → **PROBE**.

Ragdoll (Reported + PROBE):

- Reported (Acheron-NG, local pin `59f7446`, Apache-2.0 per
  `docs/REFERENCE_RESEARCH.md`): lethal-damage interception happens
  before death commits; defeat lifecycle uses `BleedoutStart`/
  `BleedoutStop` idles with a guarded state machine.
- Ragdoll body/constraint handles and the keyframed↔ragdoll switch
  entry points were not read this run → **PROBE** (candidate surface:
  `Actor` state + per-bone `hkpRigidBody` set behind the animation
  graph; exact setters need a live-probe or header pass).

Divergence rule: the leader owns graph events (replayed by followers via
`NotifyAnimationGraph` tag + payload, never invented locally) and owns
ragdoll entry/exit; a follower must suppress local ragdoll entry or its
Havok bodies will integrate a different trajectory than the host's
snapshot stream. The suppression hook is a **PROBE**.

## 4. Save/load and persistence

Verified (local), `include/RE/B/BGSSaveLoadGame.h`:

- Singleton: `GetSingleton` ID `(516851, 403330)`.
- Lifecycle flags: `Flags::{kLoading, kSaving, kPositioningPlayer, …}`
  at `+0x340`; `sizeof(BGSSaveLoadGame) == 0x348`.
- Serialization maps in one place: `worldspaceFormIDMap +0x030`,
  `formIDMap +0x298`, `saveLoadHistory +0x300`, `saveLoadChanges +0x330`.
- Per-form query: `GetChange` ID `(34655, 35577)`.
- NOTE: this class is `BGSSaveLoadGame`, distinct from the
  `BGSSaveLoadManager` Save/Load IDs carried in DEEPENING_PLAN E13
  (Reported, 1.6.1170 lineage) — do not mix the two ID sets.

Contract:

- The load/save boundary is an authority barrier: no quest, physics, or
  camera writes cross it except via ledger replay after `kLoading`
  clears. Blindly copying live files is unsafe because
  `pluginList`/`formIDMap` remap FormIDs per save — a follower applying a
  foreign file gets a different ID space.
- Leader-authoritative checkpoint = leader's save file + the campaign
  ledger's committed transactions since that save; followers load the
  same plugin list, then replay the ledger. Player-specific state
  (inventory, stats) vs shared state (world, quests) split at the
  per-form `ChangeFlags` level — exact flag partition → **PROBE**.
- SKSE co-save (`SKSESerializationInterface`) is the Reported vehicle
  for our own ledger data; the interface version handshake on 1.7.104 →
  **PROBE**.

## 5. Dialogue, scenes, quests, aliases, Papyrus VM

Verified (local):

- `include/RE/B/BGSScene.h`: `phases`, `actions`, `parentQuest +0x98`,
  `isPlaying +0xB0`, `ChangeFlags::kActive = 1<<31`, behaviour flags
  (`kDeathEnd`, `kCombatEnd`, `kDialogueEnd`, …). Scene phase
  start/completion conditions are data (`BGSScenePhase`), so the
  authoritative writer advances phases, never the conditions directly.
- `include/RE/T/TESQuest.h` (grep): exposes `GetCurrentStageID`; **no
  `SetStage` RelocationID in the header** → the stage-write hook is a
  **PROBE** (this is the single most blocking probe in this section).
- `include/RE/T/TESTopicInfo.h` (grep): `GetDialogueData(Actor*)` —
  the read seam for follower subtitle/presentation mirroring.
- Repo `Code/server/Services/QuestService.cpp:31–92` (read this run,
  dirty worktree — production code, not modified): leader-only gate
  (`IsPlayerInParty && !IsPlayerLeader` → reject), status gate, hard
  `TransactionId == 0` reject, ledger key `quest:{player}:{txid}` via
  `GetCampaignLedger().Commit`, duplicate-drop on `!commit.Inserted`,
  rebroadcast with the same `TransactionId`. This is the implemented
  single-writer + idempotency pattern the native hooks must feed.
- Reported: PR #848 quest-dedup/scene tracking and #769 quest relay
  (DEEPENING_PLAN E4/E5/E11/E12); Champollion `fd3798c` + xEdit QUST
  record graph for stage→fragment binding (see
  `docs/REFERENCE_RESEARCH.md`).

Contract:

- One writer: only the leader's game may commit scene/dialogue/quest
  changes; followers render presentation (dialogue lines, scene
  positions) read-only from snapshots.
- Join snapshot: quest stages + active scenes (`isPlaying`, current
  phase) + alias fills + open dialogue state. Alias-fill accessor and
  VM running-stack capture → **PROBE**.
- Latent-stack hazard: Papyrus latent functions (`Utility.Wait`,
  `OnUpdate` chains) resume on whatever machine runs them — a follower
  that executes fragments will double-run effects. Fragments must be
  suppressed follower-side; the suppression point (VM dispatch vs
  quest-event gate) → **PROBE**.
- Ordering/idempotency: every quest/scene commit carries the leader's
  `TransactionId`; receivers keep the ledger's duplicate-drop
  semantics. No timing-window dedup.

## Engine-integration contract (draft, none verified end-to-end)

(a) One writer per domain (physics, camera, animation, save, quest).
(b) Transitions only at sanctioned boundaries: camera `Begin`/`End`,
    save/load flag edges, ledger commits.
(c) Followers apply snapshots through read seams; they never call native
    setters that lack a pinned 1.7.104 ID.
(d) Every hook cites commit + license + header path + RelocationID, or
    is marked PROBE.
(e) Vehicles/carts are one consumer of the physics contract, not a
    parallel design.
(f) No intro-quest-specific patch is presented as universal; MQ101 stays
    the host-cinematic instance of the generic camera/scene contract
    ([INTRO_MQ101_MULTIPLAYER](INTRO_MQ101_MULTIPLAYER.md)).

## Prioritized native hook/probe slices

- **P0-1 Camera state observe** — read `currentState->id` + `cameraRoot`
  world transform via the 10 Hz bridge snapshot. Acceptance: F9-equivalent
  report shows state id transitions during any cinematic.
- **P0-2 `SetState` address probe** — resolve generated `Offset` for
  `TESCamera::SetState` on 1.7.104; until then, follower camera is
  observe-only.
- **P0-3 Quest stage-write probe** — locate the `SetStage`-equivalent
  entry point and its ID; gates the entire §5 writer path.
- **P0-4 Save/load flag gate** — snapshot `BGSSaveLoadGame::flags`
  across a save/load cycle; prove the authority barrier is observable.
- **P1-1 Physics snapshot probe** — `GetTransform` (slot `0x3A`) +
  `GetWorldScale` reads on a known movable; velocity/motion-type IDs
  follow.
- **P1-2 Animation event probe** — confirm event struct layout +
  `ProcessEvent` slot on 1.7.104; replay one `NotifyAnimationGraph` tag.
- **P2-1 License/commit reconciliation** — pin TiltedEvolution `dev`
  SHA; resolve powerof3/CommonLibSSE vs upstream header drift; record
  SKSE/SkyUI/DisplayTweaks licenses before any code reuse.
- **P2-2 Cart-as-example check** — find `bhkCartTether` in the 1.7.104
  RTTI dump and map it onto the §1 contract; no cart-specific protocol.
