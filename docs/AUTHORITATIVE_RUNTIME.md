# Authoritative runtime for a seamless Skyrim campaign

Target: every player observes the same **leader-authored world state at the
same presentation timestamp**, while keeping an independent controllable
player and camera after cinematic gates. Network transport cannot make two
machines display an event at literally the same wall-clock instant; the
acceptance condition is equivalent state on a common timeline without visible
snaps, double actions, or lost progress.

## What runs where

| Domain | Simulation authority | Follower responsibility |
| --- | --- | --- |
| Quest/scene/Papyrus effects | Leader Skyrim process | Apply sequenced effects; do not commit independent shared effects |
| NPC AI, combat, inventory, death | Leader for its loaded cells; epoch-leased simulator elsewhere | Present authoritative actor state and send player intent |
| Havok objects and ragdolls | Same cell simulator | Present timestamped transforms/poses; local collision may predict but cannot commit shared outcomes |
| Voice, subtitles, title/loading cues | Actor or scene owner | Present owner-stamped cues on the common timeline |
| Player input and private UI | Each player | Predict own movement; reconcile with authoritative outcome |
| Campaign persistence | Server transaction ledger plus leader checkpoint | Import committed state, retain personal character data |

The existing dedicated server is a sequencer, relay, and persistent ledger—not
a full Skyrim engine. Moving it to another machine does not make Papyrus,
Havok, AI packages, or cinematic scenes run there. A true headless Skyrim
server would be a separate engine implementation, not a configuration change.

## State/event contract

Every replicated domain needs `(campaign, authorityEpoch, cellLeaseEpoch,
sourceId, sourceTick, sequence, payload)`. A follower rejects stale epochs and
sequences. On joining a cell it installs a **snapshot**, buffers subsequent
events, then releases them in source order; it never starts with a local scene
and hopes to catch up. On a lease transfer, the old simulator stops committing
and the new simulator starts from the last acknowledged snapshot/sequence.

Presentation is buffered behind the shared clock. Current remote actor
movement/animation and voice/subtitles use a 300 ms delay, but camera, scene,
physics objects, ragdolls, and loading cues do not yet share a verified
post-simulation/pre-render application boundary. That—not packet bandwidth—is
the immediate parity blocker. The leader's game still owns the simulation;
followers must become view/proxy consumers for shared state.

## Engine seams to validate in order

No production rule in this plan may depend on MQ101, a cart form ID, a
particular quest stage, or the character-creator menu. MQ101 remains a harsh
regression fixture; this milestone stops at character creation until the
shared cinematic matches on both PCs.

1. **Observe the correct phase.** Capture leader camera, scene actions,
   reference bodies, actor bone transforms, and ragdoll bodies after their
   respective native evaluations. Timestamp in the shared clock. Paired
   diagnostics must compare *the same source tick*, not two sequential SSH
   snapshots a second apart.
2. **Present without feeding back.** Apply follower visual proxies after local
   animation/physics evaluation but before rendering, leaving local collision
   and native scene waits safe until each can be replaced. Verify by paired
   screenshots and transform checks, not by one root-position number.
3. **Suppress duplicate simulation effects.** Gate shared Papyrus/scene action,
   AI decision, inventory, death, and quest writes on followers while allowing
   host-sequenced applies. Existing quest-stage guarding covers only one
   native setter; it does not stop latent Papyrus stacks or scene actions.
4. **Reconcile player-owned actions.** Transmit intent to the simulator,
   predict only reversible local motion, and correct smoothly. NPC dialogue
   and door transitions need explicit leases/queues so two players cannot
   commit conflicting actions.
5. **Persist and recover.** Commit checkpoint plus replayable event tail,
   distribute personal save components at a versioned boundary, and test
   reconnect/host migration under stale and reordered packets.

Each seam remains experimental until it survives repeated two-PC runs through
MQ101, loading, character creation, combat, death/revive, doors, and saves.
After correctness, measure per-cell CPU, bandwidth, and frame time with five
players; use relevance filtering, changed-bone masks, quantization, and LOD
for high-rate pose/physics streams.

### Generic first-divergence trace

The game snapshot now includes process-local `papyrusNativeDispatch` count,
last function-name hash, and timestamp from the existing native dispatch hook.
The same-tick comparator can use changes in that counter alongside scene
action flags, camera transforms, actor pose checksums, and Havok-body state.
That counter is only one VM boundary: it does not cover Papyrus bytecode,
latent tasklets, or engine-native scene effects, so it cannot establish VM
equivalence by itself. This build has compiled and passed local tests, but has
not been deployed to either PC or validated in-game.

The next trace slice is read-only and generic: VM update/tasklet lifecycle,
pre-Havok input, post-ragdoll evaluation, and post-camera update, each stamped
with a local monotonic clock and later correlated to the shared network tick.
Physics/animation callbacks may run on worker threads; they must write only
fixed-size atomic records and must not touch ECS, network, logging, or Skyrim
containers in the callback. The candidate Precision AE callsites were
byte-matched on the installed 1.7.104 executable, but no new hook is enabled
until its ABI and crash behavior are live-validated. A first-divergence trace
is prerequisite evidence for a universal authority patch, not the patch.

### 2026-09-23 paired authority result

The deployed generic active-scene probe sampled all playing scenes, including
their action-state signatures, without relying on a quest-specific production
rule. At matched ticks in two fresh campaigns, the MQ101 scene sometimes had
identical phase and action signature while the cart references were already
roughly 60-350 game units apart and the camera roots were more than 100 units
apart. In a later sample, the leader finished that scene while the follower
remained at raw phase 9 even though both quest stages were 37. Therefore
matching quest stage, scene phase, and action flags are not sufficient to make
two native Havok/scene evaluations equivalent.

The follower's received host physics target for cart reference `0x000B9DF3`
was present and equal to the leader reference position in three sampled frames
(target age 95-102 ms), while its own reference was 176-255 units away. The
target arrives; the unimplemented safe render/physics authority boundary is
the blocker. This **does not** validate replay of all objects: the snapshots
are sparse and the cart was only a regression fixture.

The later run crashed at `SkyrimTogether.exe+0x7237D2`, reading `0x8` from a
null `RCX` (`cmp qword ptr [rcx+8],0`). The same address had appeared in an
earlier one-off dump. The caller and ownership of the null pointer are not
yet identified; do not attribute it to the new read-only probe or call this
build stable without a repeat test. The dump is in the local game directory
as `crash_UTC_2026-09-23_14-06-42.dmp`.

A source-backed, observation-only VM update/tasklet vtable hook now records
call counts and durations in the same game snapshot. The 2026-09-23 paired
MQ101 run validated that both counters advance in-game without an immediate
crash. During the cart sequence, VM Update kept advancing while UpdateTasklets
remained at its startup count on both PCs; tasklet cadence alone cannot
explain ongoing cart drift. Compare per-window changes rather than cumulative
counts from different process launch times. This does not close the earlier
native crash investigation or establish long-run stability.

## Paired MQ101 acceptance baseline (2026-09-23)

The test bridge now retains bounded snapshots by shared world tick. The
comparison asks the follower for its sample nearest the host's tick, rather
than treating two sequential SSH responses as simultaneous. In one run with
tick differences of -19 to +46 ms, the two MQ101 carts still differed by
111–313 game units. Later in the same run, both games reached quest stage 26,
but the host scene was at raw phase 26 and the follower at 19; the camera
roots were 69 units apart. These are real simulation/presentation differences,
not a one-second measurement offset. The diagnostic's raw phase is an
observed runtime field, not a safe mutation API.

Acceptance for the next authority slice: repeatable two-PC launch; no crash;
matched quest/scene events and actor states on a common tick; cart/reference,
Havok-body, bone, and camera parity under measured thresholds; no visible
teleport or stall. A passing quest stage alone is not a passing cinematic.

The one-shot paired pose capture at
`Tools/InGameTests/artifacts/authority-20260923-095225.json` sampled both PCs
63 shared-tick milliseconds apart. All 12 overlapping actors exposed readable
contiguous evaluated local poses with equal transform counts (53 or 99), yet
none of the pose checksums matched. The graph descriptors matched 12/12 but
transition signatures matched only 3/12. Both MQ101 stages were 15, while the
two carts were 273 and 214 game units apart. This identifies a viable host
pose read surface and rules out matching animation event names/variables as
full visual authority. It does not yet establish a safe follower pose write
surface or exact numeric bone error; a 63 ms sampling offset can alter hashes.

A stronger persisted one-shot capture at
`Tools/InGameTests/artifacts/authority-20260923-100252.json` sampled the two
evaluated poses 74 shared-tick milliseconds apart. Both PCs reported MQ101
stage 15 **and matching active-scene phase/action signatures**, yet cart
reference errors were 144 and 133 units, camera-root error was 121 units,
and the worst local-bone rotation difference was about 90 degrees (local
translation difference up to 5.50 units). The 12 contiguous local-pose arrays
were readable on both PCs and none matched. A full numeric pose dump is kept
only for this one-shot diagnostic; continuous sampling would distort the game.
This isolates another authority boundary: scene agreement does not imply
animation evaluation, rigid-body motion, or camera agreement.

The following paired run also found the same scene phase but a different
runtime flag word on one phase-eligible action (scene `0x000BECD4`, phase 5,
action index 20). A later sample had host phase 19 with two eligible actions
while the follower was still at phase 18 with one different action. These
observations are read-only: the flag bits are not yet proven to be a safe
serialization contract, but they establish that copying a phase number alone
would omit action state and side effects.

## Rejected shortcuts

- Copying the leader's save does not copy live Havok state, animation phase,
  Papyrus latent stacks, or audio playback.
- Forcing `TESObjectREFR` or a dynamic body's position every frame already
  made the MQ101 carts jump worse in paired trials; it is not a universal
  authority seam.
- Video-streaming the leader's single camera can copy pixels, but cannot give
  each player an independent in-world camera or interaction view.
- Moving the current relay server to a third PC does not turn it into the
  Skyrim simulation authority.

Primary models: [Quake III server snapshots](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/server/sv_snapshot.c),
[Source interpolation history](https://raw.githubusercontent.com/ValveSoftware/source-sdk-2013/master/src/game/client/interpolatedvar.h),
[SkyMP remote form view](https://raw.githubusercontent.com/skyrim-multiplayer/skymp/main/skymp5-client/src/view/formView.ts),
and [CommonLibSSE-NG native animation/scene layouts](https://github.com/CharmedBaryon/CommonLibSSE-NG).
