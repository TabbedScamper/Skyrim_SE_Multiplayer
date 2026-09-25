# Native authority: live validation record

This note records observations demonstrated on the project's supported Skyrim
runtime, separately from layouts inferred from an upstream header. It is not a
claim that the remaining animation/ragdoll replication work is complete.

## 2026-09-22 two-PC validation

Runtime under test: SkyrimSE 1.7.104.0, party leader plus one follower.

### Camera layout correction

**Verified live:** `PlayerCamera` must not redeclare the transform/root/state
members inherited from `TESCamera`. Removing the duplicate block in
`Code/client/Games/Skyrim/Camera/PlayerCamera.h` changed reads back to the base
layout and stopped the follower cinematic camera from going wild in the Helgen
cart sequence.

Evidence and cross-check:

- This repository's `TESCamera` wrapper already stores rotation, translation,
  zoom, camera node, and state in the base class.
- CommonLibSSE-NG commit
  `b93280e832f263dbef44e44cbe2936622a02f91a` (MIT),
  `include/RE/T/TESCamera.h`, places `cameraRoot` at `0x20` and `currentState`
  at `0x28`, with `sizeof(TESCamera) == 0x38`.
- The same commit, `include/RE/P/PlayerCamera.h`, begins actual derived state
  after the base and singleton base; it does not repeat the `TESCamera` fields.

**Universal rule:** replicate a camera-state identifier plus the actual
`cameraRoot` / child `NiCamera` world transform and FOV. Do not infer a
cinematic from player position, and do not use quest-specific camera patches.
The CommonLib `cameraStates[13]` layout is a candidate discriminator, not yet a
runtime-1.7.104 guarantee. Add a live probe that logs `currentState` identity,
camera-root transform, child-camera transform, world FOV, and transitions.

Candidate native seams from CommonLibSSE-NG `b93280e...`:

- `TESCameraState` virtual `GetRotation` slot `04` and `GetTranslation` slot
  `05`; the state object contains its owning camera at `0x10` and state ID at
  `0x18` (`include/RE/T/TESCameraState.h`). These are preferable read seams to
  guessing offsets inside individual animated-camera subclasses.
- `TESCamera::SetState` is RelocationID `(32290, 33026)` in
  `include/RE/Offsets.h`. The AE ID `33026` is a **probe candidate** for state
  transitions, not yet demonstrated on 1.7.104.
- `NiCamera` inherits the `NiAVObject::world` transform candidate at `0x7C`.
  Its non-VR world-to-camera matrix begins at `0x110`, and frustum/viewport
  runtime data begins at `0x150` (`include/RE/N/NiCamera.h`). Validate these
  live before writing.

Camera replication should not call `SetState` every network frame. On a host
state/epoch transition, select the matching follower state once; during the
epoch, publish host translation/rotation/FOV and drive the follower camera root
or a dedicated remote-camera state from buffered snapshots. Repeated native
state transitions would retrigger `Begin`/`End` side effects.

### Correct motion-type seam

**Verified live:** the working follower kinematic conversion is
`NiAVObject::SetMotionType(uint32_t, bool, bool, bool)` using Address Library
ID `77866` on this AE/1.7.x runtime. The previous use of ID `76033` with a
two-argument `TESObjectREFR` ABI was invalid for this runtime and crashed the
follower. With ID `77866`, the four-argument ABI, and the reference's loaded
`NiNode`, the follower cart stopped locally diverging and the user reported
cart/camera playback as correct.

Cross-check: CommonLibSSE-NG commit `b93280e...`,
`include/RE/N/NiAVObject.h`, exposes
`SetMotionType(uint32_t, bool=true, bool=false, bool=true)` on the scene object.

**Universal rule:** world references remain the canonical network identities,
but follower simulation ownership is changed at the loaded scene object's
collision layer. Host dynamic objects simulate; follower representations are
keyframed before snapshots are applied. Motion-type transitions must be
restored when authority changes or the reference unloads.

### Safe scene transform layout

CommonLibSSE-NG `b93280e...`, `include/RE/N/NiAVObject.h`, gives the non-VR
candidate layout below:

| Member | Candidate offset |
| --- | ---: |
| `parent` | `0x30` |
| `collisionObject` | `0x40` |
| `local` (`NiTransform`) | `0x48` |
| `world` (`NiTransform`) | `0x7C` |
| `previousWorld` | `0xB0` |
| `userData` (`TESObjectREFR*`) | `0xF8` |
| object size | `0x110` |

These offsets are **source-backed candidates**, not yet live-validated for
1.7.104. Before exposing them in production wrappers, a read-only probe must:

1. compare `userData` with the known reference owning the node;
2. compare `world.translate` with a stationary reference and then a moved one;
3. compare `previousWorld` across two frames;
4. reject non-finite transforms and report RTTI/collision type;
5. perform mutations only through a game-thread/task-queue seam.

The logical `TESObjectREFR::position` is insufficient for Havok-driven objects:
the visible/collision scene transform can change while logical reference data
lags it.

## Loading-screen presentation authority

Skyrim's loading presentation is split across two menus and should be treated
as its own synchronized presentation domain:

- `LoadingMenu` owns the eligible `BSTArray<TESLoadScreen*>` at AE runtime-data
  offset `0x58` relative to its runtime block (`0x40` in the AE/VR-relocated
  layout). The form identity is `TESLoadScreen` / `LSCR`, form type `0x51`.
- `TESLoadScreen` owns the selected NIF, initial scale, rotation constraints,
  translation offset, camera path, and loading text
  (`include/RE/T/TESLoadScreen.h`).
- `MistMenu` owns the instantiated `loadScreenModel`, `cameraPath`, path node,
  controller/sequence, camera FOV/rotation, and `showLoadScreen`. CommonLib's
  non-VR candidate offsets are `loadScreenModel +0xF0`, `cameraFOV +0x100`,
  `cameraRotate +0x110`, and `showLoadScreen +0x135`. Its singleton candidate
  is RelocationID `(519827, 406370)`.

These layouts come from CommonLibSSE-NG `b93280e...` and require 1.7.104 live
validation. The universal protocol should publish a `LoadingPresentation`
record when the host chooses it: plugin-qualified `TESLoadScreen` form ID,
loading epoch, chosen tip/text key if applicable, and initial model/camera
parameters. The follower must constrain `LoadingMenu` selection to that form
before `MistMenu` instantiates the model. After creation, model rotation/input
may remain local only if exact visual parity is not required; otherwise stream
the host Mist camera/model transform under the same epoch. Do not copy a NIF or
pick an array index: mod load order can reorder the eligible array, while the
plugin-qualified form identity remains canonical.

First probe: on both PCs log the Loading Menu open event, every eligible LSCR
form ID, the selected `MistMenu::loadScreenModel->userData` if present, camera
path/model names, FOV/rotation, and epoch. This identifies the still-unknown
native LSCR selection call without modifying load state.

## Actor skeleton and ragdoll authority gap

**Verified local architecture:** current actor sync sends behavior-graph
variables/events and actor transforms, but it does not serialize the evaluated
bone palette or Havok ragdoll bodies/constraints. Therefore assigning an NPC
to the party leader cannot by itself make bones or death poses match.

Treat these as distinct authority domains:

| Domain | Host publishes | Follower behavior |
| --- | --- | --- |
| Locomotion | actor transform, velocity, graph descriptor/variables/events | normal remote graph playback with correction |
| Animated pose | graph state/event sequence plus discontinuity marker | evaluate locally; use periodic bone checksum to detect drift |
| Ragdoll transition | transition epoch, entering/exiting flag, root transform | disable conflicting local transition before applying epoch |
| Active ragdoll | stable body identity, body transforms/velocities, constraint/sleep state | keyframe or otherwise make bodies non-authoritative; interpolate host pose |
| Return from ragdoll | final pose/root, graph re-entry event, epoch | apply final pose, restore graph and follower collision policy atomically |

Minimum universal acceptance probe:

- Enumerate every actor's animation skeleton nodes and every attached ragdoll
  rigid body/constraint using stable per-skeleton indices/names.
- Log graph descriptor hash, active graph index/state-machine state, ragdoll
  transition state, bone checksum, and rigid-body checksum on both PCs.
- Trigger one knockdown, one lethal ragdoll, one resurrection, and one cell
  unload/reload. A passing implementation retains the same authority epoch and
  converges both body and bone checksums without a quest-specific rule.

### Concrete graph-to-ragdoll traversal candidates

CommonLibSSE-NG `b93280e...` exposes a complete candidate traversal that is
more useful than searching actor scene nodes by name:

- `BShkbAnimationGraph::characterInstance` at `+0xC0`;
  `boneNodes` (`BSTArray<BoneNodeEntry>`) at `+0x160`, each entry beginning
  with its `NiNode*`; `rootNode +0x218`; generator outputs `+0x220`;
  `physicsWorld +0x238`; `numAnimBones +0x240`.
- `hkbCharacter::ragdollDriver +0x30`, `setup +0x50`, behavior graph `+0x58`,
  `worldFromModel +0x88`, `poseLocal +0x90`, and `numPoseLocal +0x98`.
- `hkbRagdollDriver::ragdoll +0x88`.
- `hkaRagdollInstance`: `rigidBodies +0x10`, `constraints +0x20`,
  `boneToRigidBodyMap +0x30`, and ragdoll skeleton `+0x40`.
- `hkbCharacterSetup`: animation skeleton `+0x20`, ragdoll-to-animation
  mapper `+0x28`, animation-to-ragdoll mapper `+0x30`, and unscaled animation
  skeleton `+0x48`.

`BShkbAnimationGraph` also implements `BSIRagdollDriver`; candidate virtual
seams are `01 HasRagdoll`, `02 AddRagdollToWorld`, `03
RemoveRagdollFromWorld`, `05 ResetRagdoll`, `07
SetRagdollConstraintsFromBhkConstraints`, `08 SetMotionType`, `0A
ToggleSyncOnUpdate`, and `0C ToggleConstraints`. These layouts/slots must be
read-only probed on 1.7.104 before use.

The project's current local `Havok/hkbCharacter.h` does not match the pinned
CommonLib layout and should not be extended in place without correction: it
pads to `0xA0` before declaring `characterContext`, whereas the native
candidate object is exactly `0xA0` and holds the fields above. Likewise the
local `BShkbAnimationGraph` wrapper hides the `boneNodes`, ragdoll driver, root,
and generator-output seams. A safe first patch is therefore a corrected
read-only wrapper plus layout assertions and debug checksums; do not write
through the current opaque/incorrect definitions.

### Read-only actor-pose probe implementation

`GameTestService::game_snapshot` now emits a bounded top-level
`actorPoseDiagnostics` array for the player and high-process actors. The probe
uses isolated, non-owning views from `Havok/ActorPoseDiagnosticViews.h`; each
view has compile-time assertions for the pinned CommonLib offsets, and every
native pointer/array is checked with `VirtualQuery`, count/capacity limits, and
finite-value checks before traversal. It reports graph/project identity,
active graph index, behavior/root-state-machine observables, quantized skeleton
checksum, ragdoll body/constraint/map counts, body checksum, motion-type
counts, and a transition-observables signature. Checksums omit process-local
pointer values so two-PC captures remain comparable.

This is diagnostic evidence only: `runtimeCandidate` remains `1.7.104` until
the fields are observed live. It does not write animation or Havok state.
The old local `hkbCharacter::characterContext` declaration remains a known
blocker: it begins at `+0xA0`, while CommonLib asserts that `hkbCharacter`
ends at `0xA0`, and production animation resend currently reads it as a
fallback. This patch intentionally does not change that live behavior path;
removing/replacing the fallback requires a separate in-game regression test.

Existing prior art sets the boundary clearly: TiltedEvolution's animation-mod
documentation says animations can still fail to synchronize, so its graph
variable/event path is useful but is not evidence of ragdoll-pose parity:
https://github.com/tiltedphoques/TiltedEvolution/blob/dev/README-ANIMATION-MODS.md

Precision exposes a pre-physics-step callback and Havok contact callbacks,
which supports a safe timing probe for physics state, but does not supply a
network authority model:
https://github.com/ersh1/Precision/blob/main/src/PrecisionAPI.h

## Immediate implementation order

1. Add read-only 1.7.104 validation for `NiAVObject` world/collision/user-data
   offsets and camera state/root/child transforms.
2. Add an actor pose diagnostic stream: graph identity/state, bone checksum,
   ragdoll body count/checksum, and transition epoch. Do not network full poses
   until identities and safe physics timing are demonstrated.
3. Add the universal ragdoll authority state machine and snapshots; test
   knockdown/death/resurrection plus unload/reload before expanding bandwidth
   or prediction.
# 2026-09-22 continuous loose-object authority trial

The paired intro trial of per-frame eased reference correction reduced the largest observed cross-PC movable-static offset to roughly 35–45 game units, but the follower still saw carts teleport repeatedly. Host/follower screenshots from 22:18 also showed different cinematic moments despite a shared campaign and authority epoch. This **does not validate** the correction as seamless or shippable. The tested binaries were rolled back on both PCs to the prior hashes (`SkyrimTogether.exe` `2A2B424D...`, `STServer.dll` `66AA64D6...`).

Conclusion: setting `TESObjectREFR::position` followed by `Update3DPosition(true)` fights the intro cart's own Havok/script motion even when the reference is keyframed. The next native seam to validate is the rigid body's transform/velocity and its animation/script owner, with the client render pose decoupled from collision authority. Do not re-enable camera root/world writes or deploy the experimental object easing path until a paired test shows no visual teleports and no frame-time regression.

## 2026-09-23 scene-timeline authority slice

The pinned CommonLibSSE-NG `BGSScene` layout's `isPlaying +0xB0` and raw
word at `+0xBC` were sampled on both PCs during a fresh MQ101 intro. For
scene `0x000BECD4`, the raw word moved from `0xFFFFFFFF` while idle through
small ascending values as the scene played, consistent with the proposed
phase-index interpretation in upstream PR #103. This is a live observation,
not permission to write that field. Both machines still executed native
quest-stage writes with `ScopedQuestOverride=false`, proving the follower's
quest/scene logic was not read-only.

The new `SceneTimelineService` publishes leader scene transitions with a
party epoch and transaction ID. The server rejects nonleaders/stale epochs,
orders accepted transitions, and relays them to followers. The follower logs
local-vs-host state and sample age only; it does not drive `BGSScene`, Papyrus,
animation, cart Havok, or the title menu. The first paired trace received
ordered host scene phases on the follower (server sequences 1–13), including
MQ101 `0x000BECD4`; the follower's cached native phase sometimes differed.
No claim of visual parity follows from this diagnostic stream.

Raw wall-clock timestamps from the two Windows machines are not calibrated.
Earlier apparent 2.7-second title-menu open and 8.5-second close offsets
must not be used as drift measurements. Host-sequenced observations and
follower-local timing are the valid comparison basis.

The headless `scene-authority` protocol scenario now covers ordered leader
relay plus rejection of follower-authored, stale-epoch, and duplicate
transactions. It passed alongside the other protocol scenarios on a fresh
local server (2026-09-23); the encoding suite passed 94 assertions in 10 test
cases. These establish transport/authority behavior, not in-game playback.

## 2026-09-23 follower native stage-authority candidate

The client now has a buildable guard at native `TESQuest::SetStage`: during a
leader-owned campaign, a follower's unscoped write to a syncable quest returns
false, while sequenced host applies marked by `ScopedQuestOverride` and
explicitly local-only quests pass through. This addresses one demonstrated
source of follower-local advancement, but the changed client binary has **not
been deployed or exercised in-game**. It must not be treated as a visual parity
fix. The paired gate test must check that host stages actually advance on the
follower, that the intro does not stall, and that scene/voice/cart divergence
does not increase before any broader rollout.
The preceding live build did log MQ101 host-relayed stage writes with
`override=true` and follower-originated writes with `override=false`, so the
existing override marker distinguishes these two observed paths. It does not
prove every native quest write takes this hook.

The first paired run with the stage guard kept MQ101 advancing on both PCs and
suppressed follower-local stage attempts, while host-relayed stage writes still
passed. It did **not** align the carts or camera: cart errors fluctuated from
roughly 6 to 80 game units during the sampled intro.

Two later disposable rigid-body trials failed acceptance. A follower velocity
servo produced repeated cart errors of roughly 50–340 units and follower body
velocities above the host's. A direct native body-position playback produced
even larger cart errors (over 400 units in an early paired sample) and a
follower cart frame-speed peak near 9,568 game units/s. Both paths are disabled
in the deployed client. The host stream still carries dynamic body observations
for diagnostics; do not claim they are smooth authoritative playback. The
paired Havok transform probe confirmed a 70:1 game-unit-to-Havok-unit ratio
on the MQ101 carts and that cart references reflect their body positions.
# 2026-09-23 timed voice/subtitle presentation

Both client and server were rebuilt with owner-stamped voice/subtitle packets
and deployed to the two PCs (client SHA256
`771305E9DBCC8612C1070A6E45D0EFC3D48B2BD137AA356F155532E4CF656F85`, server
SHA256 `A61512D8E334BEC9B4ADB87F804A61CF6C0289841244197E7EA19AECAC5E3412`).
The fresh MQ101 paired run reached stage 25 on both clients. Follower voice
replay logs included `(hostTick,presentationTick)` pairs
`(115179890,115179984)`, `(115190899,115190938)`, and
`(115199358,115199479)`: all played after the owner's timestamp had crossed
the same 300 ms presentation buffer used by remote actor animation. The 39–121
ms additional lateness is observed in this run, not a latency guarantee.
Serialization tests passed (106 assertions/10 cases), and protocol scenarios
including five-player join/start, scene, physics, and quest authority passed.
No player listened to this build, so audio perceptual quality and subtitle
timing remain unconfirmed. The cart error remained roughly 80–116 game units
in sampled phase-matched moments; at stage 25 the active intro scene also
showed host phase 28 versus follower phase 25. This build is not 1:1.
Both clients later reached the RaceSex character creator at MQ101 stage 75.
Native screenshots showed their independently positioned carts behind the
character models; the follower's cart occupied a much larger portion of the
background than the host's. This visually corroborates the numerical cart
error. Character-creation progression cannot be tested unattended beyond the
player choice/confirmation without adding a game-input test actuator.

### Position-only camera trial

A separate disposable MQ101 run enabled position-only owner camera playback
at the existing post-state-update hook (client SHA256
`D93F832EFCC404ADA08A1765B8DCD5E31947B404919483B1219E3A87AF4E0551`).
Six early snapshots showed camera root-position error mostly 39-56 game units
once both clients loaded, but a paired visual check contradicted any claim of
parity: the follower looked mostly at sky and cart edge while the leader saw
the road and cart passengers. The two camera orientation matrices also
diverged. This trial was disabled, rebuilt, and deployed on both PCs as client
SHA256 `10B07C05DA6517F977C868DCFAC376B114AF7ABEA25478D3EAFCBDACDD94F8D1`.
It cannot be called a camera fix. Both clients were relaunched to their menus;
the failed trial is not active.
