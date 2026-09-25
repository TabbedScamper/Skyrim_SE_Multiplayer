# Havok parity: physics objects and ragdolls 1:1

Reviewer A, 2026-09-24. Every engine claim is read from the 1.7.104 exe corpus
(`EXE_CORPUS.md`, `C:\Tools\skyrim_re\sk.cmd`). IDs are Address Library IDs, and VAs assume base 0x140000000.

## Why two PCs can never simulate the same thing

Lockstep determinism is off the table. The code shows three independent reasons:

1. **Framerate-driven physics clock.** `FUN_14104ab70` (**ID 77850**) is called once per frame from the main loop
   (`FUN_140659d10`, ID 36577) with the frame delta. It sets the substep to `fMaxTime(1/60) * BSTimer multiplier`
   (or a 1/30 single-step mode). It clamps each frame to 3 substeps, discards anything under 1 ms, and carries the
   remainder into the next frame (`DAT_143336518`). Each PC slices time differently from its own framerate, and
   below 20 fps physics runs slower than real time.
2. **Multithreaded solver.** `bhkWorld::Update` (**ID 77851**) steps `hkpWorld` through Havok's async MT stepper:
   frame marker `FUN_140b5b3b0/…220/…1e0`, then `initMtStep(jobQueue, threadPool, substep)` (ID 61417), then
   `finishMtStep` (ID 61410, where our pre-step hook already sits), looping until `isSimulationAtMarker`
   (ID 61415). The class list includes `hkpMultiThreadedSimulation`. Job and island order are not reproducible
   across machines.
3. **Initial state never matches.** Streaming order, body insertion and activation order, and each PC's local AI
   all differ.

So 1:1 means **one owner simulates, everyone else replays**. The current approach can't converge. It leaves remote
bodies *dynamic* and steers their velocity, and it fights Havok with `SetPosition`/`ForcePosition` (the 191
suppressed vs 192 override calls on the corpse). That is why the cart sat 76 to 255 units off.

## What the engine gives us for free

### A complete per-reference physics-state serializer (the save-game one)
- **Save:** `FUN_1402e1810` (**ID 19534**), `TESObjectREFR` Havok data into `BGSSaveGameBuffer`. It walks every
  collision object in the ref's 3D with the visitor `FUN_14104d720` (**ID 77883**) and the per-body callback
  `FUN_1402e2230` (**ID 19537**). For each `bhkRigidBody` it writes:
  - position in game units (Havok ×69.99125)
  - rotation quaternion
  - a flags byte (bit0 active, bit1 keyframed motion type 4, bit2 …)
  - and, when active, the motion's velocities (from `motion+0x230`)
  - it also stores two actor ragdoll floats (`+0x24c/+0x250`)
- **Load:** `FUN_1402e19a0` (**ID 19535**). It sets the motion type (keyframed, type 4, for the cases it selects),
  restores each body, updates the node, and resyncs the behavior graph's ragdoll state
  ("Behavior ragdoll save/load state mismatch" is its error).

This is the engine-exact way to reproduce a body or ragdoll's full state, the same way corpses keep their pose
across save/load. **To do:** back `BGSSaveGameBuffer`/`BGSLoadGameBuffer` with memory (read their vtables with
`sk.cmd vt "BGSSaveGameBuffer|BGSLoadGameBuffer"`), so the host can serialize a ref and the follower can load it.

### Motion-type control
- `NiAVObject::SetMotionType` (ID 77866, already bound in `NiAVObject.cpp`). Type 4 = keyframed. The load
  routine above uses exactly this to pin restored bodies.

### Ragdoll post-physics
- `UpdateRagdollPostPhysics` `FUN_1400a7bf0` (**ID 6359**) and `RagdollsPostphysics` `FUN_14060d740` (**ID 35402**)
  are the per-frame ragdoll-to-skeleton pass. Relevant classes (RTTI): `hkbRagdollDriver`, `BSIRagdollDriver`,
  `bhkRagdollController`, `hkaRagdollInstance`, `hkbKeyframeBonesModifier`, `hkbPoweredRagdollControlsModifier`,
  `hkbRigidBodyRagdollControlsModifier`, `ExtraRagDollData`.
- `Actor::SetLifeState` (ID 37612) drives death and bleedout for the player's controls and camera (see `EXE_CORPUS.md`).

## Design

### 1. Physics objects (cart, clutter, thrown items)
- **Owner** (host by default, or the player who grabbed or hit it, via the existing epoch ownership extended to
  physics refs) simulates normally and publishes, per owner substep: the body transform and velocities in the same
  layout as ID 19537, stamped with the owner's physics tick.
- **Non-owners** set the body to **keyframed (type 4)** while remote-owned. In the existing pre-step hook
  (ID 61410 region), each substep, apply a *hard keyframe*: velocity = (target − current) / substep for both linear
  and angular, targeting the interpolated host transform ~100 ms in the past. A keyframed body still pushes
  players and props, but nothing pushes it back, so it can't drift. Restore the original motion type when
  ownership returns.
- On ownership transfer, send one full ID-19534 snapshot so the new owner starts from the old owner's exact state.

### 2. Physics clock alignment (removes interpolation error, not a determinism attempt)
- Hook ID 77850 on followers. Keep each PC's own slicing, but record `(host physics tick, substep)` so replay
  targets are sampled at matching physics times rather than wall-clock guesses.
- Sync the `BSTimer` multiplier (slow time, kill-cams, Dragonrend). It changes the substep on the host and has to
  change the replay rate on the follower.

### 3. Ragdolls
- **While a ragdoll is moving** (death, knockdown, a shout push): the owner publishes every ragdoll body
  (enumerate with the same visitor, ID 77883, so bodies come out in the engine's order) using the ID-19537 layout.
  That's about 44 bytes × ~15-20 bodies. Send only bodies whose active flag is set, so traffic drops to zero once the
  corpse sleeps.
- **Non-owners** let their copy enter ragdoll natively, but set every ragdoll body keyframed and hard-keyframe it to
  the owner's bodies in the pre-step hook. Then let `UpdateRagdollPostPhysics` (ID 6359) pull the skeleton from
  those bodies as usual, so there's no bone writing and no fighting.
- **When it settles** (all bodies inactive on the owner): send one ID-19534 snapshot, and the follower applies it
  with ID 19535. That gives the final corpse pose through the same path as a save reload, and survives cell
  unload through `ExtraRagDollData`.
- Remove `InterpolationSystem` `ForcePosition`/graph writes for dead remote actors entirely (already partly
  done). Positions of the dead come from the ragdoll replay only.

### 4. Gates (extend `Compare-TwoPcAuthority.ps1`)
- Per-body position/rotation error at the same host physics tick (target below 1 game unit while moving, 0 after
  settle).
- A follower body must never be dynamic while remote-owned.
- Zero `SetPosition` suppressed/override calls on replayed refs.

## Open items to read in the corpus before coding
- `BGSSaveGameBuffer` / `BGSLoadGameBuffer` layout and constructors, needed for memory-backed buffers.
- The ragdoll body list on an actor: follow what ID 19534 does for actors (`FUN_140675310` → `+0x24c/+0x250`) and
  the visitor at ID 77883.
- How `SetMotionType(4)` interacts with `bAddBipedWhenKeyframed:HAVOK` and the character controller for actors.
- Contact and impulse events (`ApplyHavokImpulse` native, hit impulses) on non-owners: forward them to the
  owner instead of applying them locally.

## 2026-09-24 prototype update (unverified in game)

- The save writer is **already memory-backed**: `BGSSaveGameBuffer::SaveDataEndian` (VA `0x140639F00`) writes to its buffer pointer at +0x08, grows capacity at +0x10 via VA `0x14063A070`, and advances used bytes at +0x14. Its destructor at VA `0x14063A250` frees that allocation. The missing work is establishing safe construction/ownership and a bounded load buffer, **not** inventing a memory writer. `BGSLoadGameBuffer::LoadDataEndian` (VA `0x140637E70`) reads pointer +0x08 and advances cursor +0x24 with no apparent bounds check in that function; do not feed it untrusted network bytes.
- The existing `set_pre_step_body_playback_probe` already had mode 3, a one-object keyframed pose probe. A source-only mode 4 (`hard_kinematic`) now computes linear and angular velocity needed to approach the latest finite owner target over one Havok substep, using `bhkRigidBody::SetLinearVelocity` (VA `0x141058D70`, ID `78089`) and `SetAngularVelocity` (VA `0x141058E40`, ID `78090`). Speed caps limit corrections, so it is **not guaranteed exact in one step**. It remains opt-in, not normal object sync and not ragdoll sync. The two-PC error gate and interaction safety have not yet been verified.
- Reviewer A and Reviewer B both rejected the first mode-4 draft because it teleported a body inside the solver. The draft was replaced with the velocity approach above. Reviewer B additionally flagged pre-existing lock-free watched-body pointer publication and restoration-on-unload hazards in the probe infrastructure. Those require a focused safety review before enabling the mode in a live physics session.
