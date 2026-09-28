ï»¿# In-engine paired harness

Build the main tree with `--harness=y`, then run:

```powershell
powershell -File C:/Tools/skyrim_re/Run-Harness.ps1 -Deploy -Scenario helgen-intro
powershell -File C:/Tools/skyrim_re/Run-Harness.ps1 -Scenario helgen-intro-assisted
```

The first scenario is the **collision-on qualification**. The second is a separate
TCL-assisted evidence run and can only return `diagnostic-passed`. To continue an
existing healthy upper-tower session, use `-ContinueSession -Scenario
helgen-tower-continuation`; this never counts as a complete new-game pass.
`helgen-road-continuation` starts from the actual completed stage130 checkpoint
and retains the prior TCL-assisted classification.

Both test compilation and `SkyrimTogetherReborn/harness.json` with
`{"enabled":true,"armor":true}` are required. Server `[Harness] bEnabled=true` is
also required. The runner prepares these settings and bAlwaysActive, arms
C:/Tools/diag/diag-arm.ps1 on both PCs before launch, forms the existing Steam
party and begins the native scenario. No action uses emulated keys.

Before starting the scenario, the hard health gate verifies one server PID,
owned UDP10578, fresh server build tag, exactly equal client tags, and fresh
`Effective Data scan complete` / `Joined shared campaign` lines on both PCs.
Failures preserve the server log tail. The server already starts in the game root at baseline25fa030d. This revision
does not establish that working directory caused an earlier failure.
Startup evidence is searched across current and three rotated client logs; the
same process-start timestamp and build-tag requirements apply to continuations.

The leader sends ordered, epoch/run/sequence checked steps. Each PC records and
acknowledges preconditions before the server releases execution; all original
participants must then acknowledge completion before the next step. Membership
or authority changes abort. Preconditions include quest stage, host-bound cell,
scoped scene-idle state, combat-clear and attached cell/player3D/load completion.
Cinematic waits/captures explicitly observe scene/combat state without requiring
it to be idle/clear. Movement requires completed cart/execution scenes idle. Through the inn landing,
movement also requires combat clear. Post-inn exterior teleport/walk steps observe
and allow combat because the escape route passes through scripted Alduin combat
volumes; the final interior finish still requires clear combat. Continuing escape
scenes are separately observed because movement advances their packages. No active scene is reported as idle.

The step timeout scales with party size (capped at 3x), observed loading (capped
at120s), a300s peer allowance, and up to300s local loading extension. The server
watchdog covers both phases. These bounds prevent indefinite waits; a timeout is
a failed run. Inbox overflow and any capture drop/I/O failure invalidate results.

Scenarios are JSON objects with `metric` (`collision-on` or `tcl-assisted`) and up
to256 steps, each at most4096 bytes. Every step includes a `preconditions` object.

| Operation | Main fields |
|---|---|
| wait_stage | quest, stage; done:true checks actual stage-completed bit |
| wait_scene_end | scene |
| wait_cell | cell; door_vote:true requires real same-vote State/Go/Release |
| wait_menu | menu, open |
| wait_seconds | seconds |
| walk | ref or x/y/z, radius, pace; step_over enables bounded native recovery; until_trigger requires local-player entry before completion |
| jump | ref, radius; existing native jump, actual target proximity required |
| follow_objective | quest, objective_id/objective_ref, optional until_cell |
| teleport | ref, optional dx/dy/dz; native SetPosition and same physical cell reconciliation |
| god_mode | enabled; native setter/getter with readback and prior-state restore |
| console | bounded allowlisted command; TCL rejected in collision-on metric |
| activate | ref; doors rejected to preserve real door-vote path |
| creator_finish | existing native creator completion |
| watch | refs and actors (maximum16 each); actor_aliases (maximum8 quest/alias pairs) |
| capture | tag in ongoing bounded stream |
| assert / finish | optional cell and door_vote; finish must end scenario |

All staging coordinates derive from esm.sqlite references, triggers and markers.
The source audit is round6/harness-route-current.json; full quest exports are
harness-mq101-current.json and harness-dragon-current.json. Stage35 staging uses
the trigger's rotated thin-axis boundary and CD697's floor. Floor and landing
suitability still require live validation. Teleports cannot cross physical
exterior grids or interior cells. Grid boundaries use real short movement and
the Keep uses the actual automatic load-door vote. No quest stage is forced.
`until_trigger` records the original local-player entry, not replicated quest
state. One-shot triggers may disable before a follower crosses; a missing entry
fails rather than inheriting the host's movement proof. Stage35 has paired entry
evidence; the new stage130 event mapping still requires fresh runtime validation.

Every tick records global TCL state, player flags, and bounded non-atomic
controller-filter reads. Collision-on tested movement fails on disabled or
unknown controller collision. The vanilla cinematic's existing pass-through
behavior is only observed. Wall chunk collision pointers/raw flags are limited
observations, not proof of complete Havok geometry correctness.

Main-thread captures enter a bounded SPSC queue; disk append/flush belongs to the
writer thread. Named-reference and armor/pose observations have explicit limits.
No screenshots, VM snapshots, whole-cell or all-actor scans occur in the harness.
Native status streams let the external runner wait without window-thread polling.
Protocol policy tests include five participants; they do not prove five-player
engine performance.
Offline frame-gap statistics exclude only the first record, which has no earlier
captured frame in that run; its raw value is retained as `firstFrameGapMs`.

Results are under round6/capture/harness-<stamp>, with paired logs, statuses,
server log, diagnostics, result.json, summary.md and offline analysis. A crash
classification needs process exit and a complete full-memory exception dump for
the launched PID. Informational406D1388 startup dumps and handled first-chance
exceptions are not crash passes. Collection failure invalidates a counted pass.


Review-round acceptance changes: SHA256 checks now bind both installations to the
same client/server build-output bundle, with loaded-module and process identities
in artifact-identity.json. This does not prove the artifacts include current dirty
source edits. Collection requires client/server logs. Analyze-Harness.py --validate
requires complete JSONL records, all ordered step barriers, actions_complete and
matching terminal status on both PCs; evidence-validation.json records its decision.
Legacy captures lacking the new identity/epoch evidence cannot be newly qualified.

Jump diagnostics now separate a requested native state from observed Jumping/InAir
and subsequent OnGround states. The original CanJump predicate only gates initial
eligibility. Native state transitions still do not prove collision-body geometry;
a jump step also requires reaching its target radius. Do not interpret an absent
sample as proof the player never left the ground.
An immediate request sample other than 1, or no air-state sample within 750ms,
does not abort movement. The native request is conditional and its state can be
consumed. Outside harness ownership, the driver retains its two-second limit and first-landing
stop. A harness jump reads its requested radius and keeps collision-enabled movement
through an early roof-edge landing, capped at four seconds after submission.
`jump_landing_approach` identifies this phase; grounded arrival requires actual target
proximity. The terminal timeout still goes through the unchanged harness proximity
check. Missing samples neither establish a physical cause nor pass the step.
Runtime results for this continuation are recorded in docs/REFERENCE_RESEARCH.md.

The explicit `party_trigger` walk mode tests the production proximity contract for
one-shot triggers. The leader must record an original local player trigger entry.
Followers walk natively to the ESM-derived `party_wait` point, within its radius
and within 400 units of the trigger or 600 units of the actual loaded leader in
the same worldspace. The leader lookup is requested through the update mailbox,
bounded to 16 player components, then resolved and sampled on the main thread.
Their distinct
`party_trigger_proximity_arrived` record is not local contact evidence. Offline
acceptance checks both roles' evidence and recomputes proximity from coordinates.
Paired preparation/completion barriers and the following quest-stage check still
apply. This exercises party-present progression, not an absent-party hold test.

Frame field previousTickCaptureUs is the preceding tick's harness duration. The
legacy captureUs field had that same meaning. A hitch frame's own duration appears
in the next record, and this timing alone cannot identify a hitch's cause.

The Headsman watcher uses MQ101 alias47, allowing each PC's runtime ID to differ.
Actor/alias/node work is bounded; quest discovery is time-sliced. Status JSON is
built at4Hz or on significant changes. Server harnesses permit at most16 members
and retain at most64 party histories per server process. No5/10-player runtime
capacity is claimed. See REFERENCE_RESEARCH.md for full costs and native IDs.
