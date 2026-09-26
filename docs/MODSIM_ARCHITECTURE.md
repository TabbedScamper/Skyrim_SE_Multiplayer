# ModSim architecture

ModSim is a deterministic, headless multiplayer model for Skyrim SE
Multiplayer. It does not claim to emulate Skyrim. Its job is to find protocol,
authority, ordering, persistence, and quest-progression failures cheaply, then
produce a small replay that the two-PC engine harness can validate.

## Confidence tiers

### Tier 1: exact headless tests

These tests execute production serialization, authentication, server services,
and party rules. Plugin records are treated as exact authored data, not as an
emulation of the engine that consumes them.

- Join, party creation, leader handoff, reconnect, and snapshot catch-up.
- Message validation, ownership epochs, transaction identity, and idempotency.
- Server-owned quest, actor, inventory, door, dialogue, and player-lifecycle
  state machines as those services are implemented.
- Packet delay, duplication, loss, reordering, disconnect, and process restart.
- QUST stages, objectives, aliases, scenes, and VMAD bindings extracted from the
  installed load order.
- Persistence projections and convergence of every simulated client on the
  committed server snapshot.

### Tier 2: executable Papyrus model with conservative native shims

The imported MIT SkyMP Papyrus VM can parse and execute PEX bytecode outside
Skyrim. ModSim supplies deterministic native-function shims for effects such as
`SetStage`, `CompleteQuest`, `Enable`, `Disable`, `MoveTo`, scene operations,
inventory changes, and control changes. Decompiled PSC remains useful for
review, but the executable PEX is the behavioral input. A shim must report
uncertainty instead of pretending to reproduce an engine native it cannot
model.

- A branch whose condition cannot be evaluated is explored both ways.
- A latent or engine callback becomes an explicitly schedulable event.
- Unknown native calls taint dependent assertions.
- Results from this tier are `possible conflict`, never proof of engine parity.

The current `SkyrimPapyrusProbe` successfully parses the installed MQ101 quest
fragment, quest controller, and player alias PEX files. Together they contain
181 functions and 1,416 bytecode instructions. Its call-surface report is the
input for implementing and prioritizing native shims.

`SkyrimPapyrusSim` now executes all 159 authored `Fragment_*` functions from
the installed MQ101 quest-fragment PEX individually without missing direct
calls or VM runtime errors. Exact logical shims cover stages, objectives,
inventory, aliases, references, enable/disable, movement, scenes, controls,
and waits. Engine-facing operations such as animation, camera shake, vehicles,
motion type, chargen UI, saves, and achievements execute through explicitly
`conservative.*` trace events until differential game traces define them.

### Tier 3: engine replay

The following remain GPU-backed tests against the installed game:

- Papyrus VM timing, latent calls, save/load behavior, alias filling, and
  condition functions that read the live world.
- AI packages, dialogue/scene arbitration, navigation, combat, detection, and
  Radiant Story selection.
- Havok, ragdolls, dropped-object motion, animation graphs, and actor visuals.
- Scaleform layout, camera, input focus/cursor clipping, audio, and rendering.

Every high-value or uncertain ModSim failure should emit a compact replay seed
and expected invariants that the existing in-game debug bridge can capture.

## Components

1. **Corpus compiler** joins Mutagen-derived QUST/SCEN/VMAD records with the
   local PEX audit and emits a normalized, redistributable quest graph. It
   stores identifiers and derived effects, not Bethesda text or assets.
2. **Deterministic world model** owns party membership, authority epochs,
   players, quest transactions, actors, references, inventories, dialogue
   leases, transition barriers, and death/down/revive state.
3. **Production adapter** drives the real dedicated server and production
   codecs where possible. Pure model implementations are used only for state
   that does not exist in production yet and are marked as specification tests.
4. **Scheduler and fault injector** runs a virtual clock and seeded event queue.
   It can delay, duplicate, drop, reorder, disconnect, reconnect, and crash a
   participant or server at any declared yield point.
5. **Scenario runner** reads versioned JSON scenarios, expands bounded choices,
   executes multiple schedules, and writes an event trace plus final snapshots.
6. **Oracle and reducer** checks invariants after every event and minimizes a
   failing schedule while preserving its seed and corpus hashes.
7. **Engine replay exporter** converts a minimized trace into instructions for
   two real clients and correlates their native snapshots with the headless run.

## First scenario families

1. **Session continuity:** create/join, reconnect, late join, leader migration,
   stale epoch rejection, and full snapshot catch-up.
2. **Quest transactions:** leader-only commits, duplicate/reordered stages,
   branching objectives, scenes, alias changes, crash recovery, and followers
   catching up without executing local authoritative fragments.
3. **Dialogue lease:** one NPC has at most one speaker; followers observe the
   host conversation and cannot start or advance it; disconnect releases the
   lease deterministically.
4. **Cell-transition barrier:** followers queue behind the host at doors and
   fast travel, load the same destination epoch, and materialize only after the
   barrier commits or times out.
5. **Player lifecycle:** lethal damage selects downed or dead based on nearby
   eligible players; revive is a single-winner transaction; inventories
   survive death; reconnect cannot resurrect stale actor state.

Actor ownership, persistent reference identity, dropped-item pickup, combat
death, ragdoll pose snapshots, and animation recovery are the next family. The
headless tier can prove authority and convergence for them, while their visual
result still belongs to engine replay.

## Core invariants

- Exactly one current leader and monotonically increasing authority epochs.
- Only committed leader transactions advance shared quest state.
- A transaction ID has at most one effect, regardless of delivery count.
- Rejoined and late-joining clients converge to the same committed snapshot.
- A persistent item or reference has one identity and one authoritative owner.
- One NPC cannot hold two dialogue leases at the same logical time.
- A cell transition cannot expose a controllable player in the wrong epoch.
- A player lifecycle has legal transitions only; revive and death cannot both
  commit for the same down event.
- Inventory conservation holds across pickup, trade, death, reconnect, and
  save projection unless an explicit game transaction consumes an item.

## Scenario contract

Each scenario records its schema version, corpus/load-order hash, random seed,
participants, initial state, timed actions, network fault policy, and assertions.
Every run records the exact expanded schedule, server decisions, client views,
uncertain Papyrus effects, invariant failures, and production build identity.

Acceptance for the first usable ModSim release is:

- the existing protocol scenarios run through the common scenario format;
- the same seed produces byte-equivalent normalized traces;
- duplicate, reorder, reconnect, and leader-loss matrices run in CI;
- injected follower quest writes and duplicate revive commits fail correctly;
- a failing run is replayable and reducible to a short event sequence;
- uncertain engine effects are labeled and exportable to the two-PC harness.

## Reviewer B's role

Reviewer B can run as a headless, read-only second reviewer. It can inspect corpus
summaries, generate candidate scenarios, mutate schedules, classify failures,
and propose minimized repros. Its output must be machine-readable suggestions
that ModSim validates; Reviewer B is not an oracle and does not approve changes on
its own. Any code-writing Reviewer B task should use an isolated worktree and still
pass the deterministic runner and normal review.
