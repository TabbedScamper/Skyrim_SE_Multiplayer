# Layer 1: recorded-run replay

This layer runs without Skyrim, remote-PC access, xmake, deployment or a commit.
It leaves all production implementations in place, including behavior present
at `25fa030d`. The new headers are offline counterparts awaiting integration by
the owners of the live paths. The two-PC harness remains final engine proof.

## Run

From the repository root in PowerShell:

```powershell
$py = 'C:/Users/mwalt/AppData/Local/Programs/Python/Python312/python.exe'
& $py Code/tests/run_replay_checks.py --prove
```

This compiles only `replay.cpp` and the actual pure `NpcInventory.cpp` in a
separate temporary directory, using the coordinator's cached compiler/library
paths and existing Catch main object. It never invokes xmake or writes `build/`.
An already built TPTests picks up `replay.cpp` through its existing `*.cpp` glob:

```powershell
$env:TP_REPLAY_FIXTURE = "$PWD/Code/tests/fixtures/replay/recorded_failures.rpl"
./build/windows/x64/releasedbg/TPTests.exe '[replay]'
```

`--prove` first runs regression checks, then requires a nonzero observed-output
run and checks its JUnit failures by exact test name. A generic compilation,
missing-fixture or unrelated assertion failure cannot satisfy that proof.
To inspect the raw acceptance failure directly:

```powershell
& $py Code/tests/run_replay_checks.py --mode observed
```

That command **must exit nonzero** for the committed failures. The default
regression mode passes when it detects those failures and the worn-plan
counterfactual succeeds; it does not claim the recorded run passed acceptance.
Set `TP_REPLAY_MODE=observed` for the equivalent existing-TPTests invocation.

Re-extract from immutable captures, across all 32 worker processes:

```powershell
& $py Tools/Replay/extract.py --workers 32 `
  --captures C:/Tools/skyrim_re/agent/captures `
  --out C:/Tools/skyrim_re/agent/replay-layer1 `
  --fixtures Code/tests/fixtures/replay
```

Full normalized streams and a source-hash/coverage manifest stay outside the
repository. The committed JSONL/binary subset is small and self-contained.
`FORMAT.md` specifies units, clocks, phases, missing measurements and binary
layout. The extractor detects files changing while it reads them.

## Recorded proof

All coordinates/items are extracted, not invented fault injections:

* Cart `BB970`: follower harness lines 4211 -> 4215 in
  `harness-20260927-103856`, ticks 234743495 -> 234743511, wall delta 15 ms.
  Distance is **152.715135782 units**, failing the <=30 step target.
* Ralof `2BF9E`, server 5, epoch 1, sequence 14: Host `tp_client.log:4271`
  publishes `0:A6D7B@4` and `0:A6D7F@80`; Follower `tp_client.log:19662`
  records `settled`, `matches false`, `local []`. The follower's line 19582
  separately reports `unavailable 2`, `ambiguous 0`.
* Actual `PlanNpcWorn` returns supply/equip operations. The in-memory adapter
  with supply omitted stays empty; applying the complete plan yields both
  recorded items and a second application requests no changes. This is a
  **presentation-plan correction**, not proof of missing native stock or a
  fix to game inventory, attachment, enchantment identity or loot authority.
  The legacy log only contains form/slot identity; extra-instance fields are
  deliberately a projection in this test. No live inventory is reconstructed.
* 244 paired reference samples in a one-second window around the cart jump:
  cart median **55.9602 units**, horse maximum **15.0495 units**, both over 5.
  These are reference-observation gaps on the network clock, not same-time
  packet/body presentation errors. They are not whole-run statistics.
* Spectate snapshot: player pitch -1.5707964 rad, camera state 11. Rendered
  camera pitch and player intent are unrecorded. The runner reports MISSING;
  it does not turn the inactive camera's zero fields into an acceptance pass.
* Complete paired ragdoll body transforms/rotations/settled state and debris
  body state are missing. Their metrics and reader channels exist; they are
  reported MISSING for this historical fixture. A three-node pose or immediate
  post-write residual of zero is not the required settled/rendered proof.

The replay reports the checking unit and source provenance on failure. It does
not pretend that a reference jump identifies its native producing function.

## Implementation and source fidelity

| File | Responsibility |
|---|---|
| `Tools/Replay/extract.py` | Parallel normalization, exact-message worn pairing, loaded/consecutive-frame step selection, short network-clock gap window, JSONL/RPL generation. |
| `Code/client/Services/ReplaySyncMath.h` | Owned-path counterparts: actor interpolation/prediction, dynamic steering, assembly goal/command cap, ragdoll quaternion interpolation/error, worn-delivery count; tracker metrics. |
| `Code/client/Services/ReplaySyncPolicy.h` | Verbatim playback-window and door-policy copies, namespaced for offline use. |
| `Code/tests/replay_recording.h` | Bounded little-endian fixture reader with explicit errors. |
| `Code/tests/replay.cpp` | TPTests `[replay]` recorded negative controls, pure-policy replay, thresholds, five/ten-peer occupancy and corruption checks. Uses existing `PlanNpcWorn`, `TriggerCapabilities`, `TriggerPartyContact` directly. |
| `Code/tests/run_replay_checks.py` | Isolated compiler/link runner, source-drift checks, exact failing-test proof, report hashes. |
| `Code/tests/replay_extractor_test.py` | Binary determinism, real source/hash/line provenance, reload boundaries and receive/apply/unknown-peer separation. |
| `Tools/Replay/source_blocks.json` | SHA-256 fingerprints of copied math regions. Drift fails closed; inspect the changed owned path before refreshing. |

`NpcInventory.cpp` is compiled from the current working tree, not an old static
copy. Compiler inputs are hashed before/after the test build. Header copies of
`SelectPlaybackWindow` and `DoorVotePolicy` are compared verbatim. Other math
blocks are pinned by source region hashes because their engine-facing inputs
need adapters; the fingerprints are not a substitute for numerical tests.

Native source read: E62478 / VA `0x140B9A9C0` computes COM-aware linear/angular
velocity and calls motion setters. Therefore a bounded target/command is not a
bound on the observed solver step. E38001 / VA `0x1406B2570` checks native
inventory count and returns when it is below one; replaying an equip alone
cannot represent materializing missing stock. References and rejected
alternatives are recorded in `docs/REFERENCE_RESEARCH.md`.

## Cost and validation limits

This adds **zero live per-frame/per-tick work** and no live allocations. Offline
normalization is O(input bytes) with 32 processes and streamed output; worn
publications are indexed by exact identity. The one-second motion extraction
reads two files once and does O(F log H + F B) work for F follower/H host frames
and B watched bodies. It is an offline fixture-building cost, not a game loop.

At a future live integration, each steering/interpolation/error operation is
O(1) per changed body; playback selection is O(S), currently S <=12. Process
dirty actor/body sets with an explicit scheduler budget: O(P * (dirty A + dirty B))
for replication, O(dirty A + dirty B) per follower. Worn planning is event-driven
O(Wlocal * Wowner), bounded by the 35-item protocol, not an actor scan each tick.
Trigger membership uses the existing event-driven hash set and O(P) storage per
occupied trigger. Tests cover P=5 and P=10 policy behavior, **not** performance
or in-game correctness at those party sizes.

COMMON.md explicitly waives local Reviewer A/B for this job. Neither reviewed
this layer; no review approval is implied. No PCs or game processes were used.

## FOLLOW-UP

1. **ObjectService owner (cartab/debrisfix):** switch to reviewed pure window,
   steering and target math only after the current job settles. Add a bounded
   adapter recording stable body/topology IDs, target/feed, host/presentation
   ticks, pre/post physics and rendered results. Preserve the 25fa030d dynamic
   steering/trigger behavior. The 153-unit observed jump is caught now; its
   actual native producer remains unresolved by these captures.
2. **CorpseRagdollService owner:** integrate quaternion/error units after the
   current rewrite; record matched body keys, position/quaternion/velocity,
   settled/falling state and measurement phase. A live edit during this task
   added velocity prediction, which is outside the copied rotation/metric
   blocks. Do not mistake these counterparts for the entire current service.
3. **NakedNpcGuard / InventoryService / NpcLootService owners:** connect the
   production pure worn plan to the actual stock/instance/completeness policy
   and replay action outcomes. Record full inventory extras and unsuccessful
   operations. Keep render-only repair separate from loot stock; the offline
   supply counterfactual does not authorize adding lootable copies in game.
4. **Harness owners:** emit the canonical channels in FORMAT.md with bounded
   changed-state records and drop counters. Legacy observations cannot recover
   missing packet/physics values. Keep frame/clock provenance and non-atomic
   flags. Do not do a whole-actor survey per tick.
5. **Camera / interpolation owners:** integrate pure timeline math and capture
   player intent, active rendered camera state, authority and explicit radian
   tolerance. Preserve scripted/spectate camera ownership. A +/-90 degree player
   rotation is not by itself a failed camera-intent match.
6. **CreatorTogether / BoundPoseKeeper / SaveLoad.cpp / Forms.cpp owners:** no
   extraction attempted while owned. Later supply creator/load boundaries and
   pose rebuild events as replay resets; do not infer them from worn arrays.
7. Once a reviewed candidate passes headless replay, the coordinator rebuilds
   and uses paired in-game observations for final proof. Replays cannot model
   Havok contacts, native ragdoll ownership, attached armor clones or rendering.
