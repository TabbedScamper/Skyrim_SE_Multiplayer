# Headless engine emulation

The target is not a renderer-free copy of `SkyrimSE.exe`. It is an executable,
trace-calibrated behavioral twin for quest and multiplayer decisions. That is
enough to explore player interleavings and find campaign conflicts while the
real game remains the oracle for presentation and native edge cases.

## Layers we can reproduce

1. **Authored records:** quests, stages, aliases, scenes, dialogue conditions,
   packages, references, inventory templates, cells, and load-order overrides.
2. **Papyrus bytecode:** state machines, properties, variables, arithmetic,
   branching, arrays, method/static calls, events, inheritance, and suspended
   native calls through the imported MIT SkyMP VM.
3. **Deterministic natives:** quest stages/objectives, reference enable/disable,
   logical movement, aliases, inventory, factions, actor values, controls,
   dialogue/scene leases, timers, and multiplayer transactions.
4. **Condition evaluation:** CTDA boolean grouping plus individually implemented
   condition functions backed by modeled world state. Unknown functions return
   an explicit `unknown`, never an optimistic true result.
5. **Abstract AI and physics:** packages, scenes, combat, animation, and ragdolls
   can expose logical states and completion/failure events without attempting
   pixel or rigid-body equivalence.

## Trace calibration

Every native shim has a confidence label: `exact-record`, `verified-trace`,
`conservative`, or `unknown`. The in-game bridge records native inputs, outputs,
state deltas, and event ordering for focused probes. Those traces become
differential fixtures: the headless result must match every observed invariant
before its confidence can be promoted.

This turns engine-only behavior into a shrinking boundary. For example, ModSim
does not need to recreate Havok to test shared death ownership. It needs a
server-authoritative death transaction, a logical corpse identity, and a
recorded pose payload. Skyrim remains responsible for rendering that pose, and
the engine replay checks the visual result.

## Current proof

`SkyrimPapyrusProbe` parses real installed MQ101 bytecode without launching the
game. Across `QF_MQ101_0003372B`, `MQ101QuestScript`, and `MQ101PlayerScript` it
found 181 functions and 1,416 instructions. The most frequent calls include
`GetActorRef`, `TryToMoveTo`, `SetStage`, `GetRef`, `Game.GetPlayer`, `Stop`,
`TryToEnable`, `TryToDisable`, `MoveTo`, `PlayIdle`, `EvaluatePackage`, `Start`,
and `Utility.Wait`. This gives the first concrete native-shim priority list for
running Helgen headlessly.

The next implementation milestone is complete: the simulator independently
executes all 159 `Fragment_*` functions in the MQ101 quest-fragment PEX. Four
representative fragments are permanent ModSim assertions for stage changes,
inventory grants, cast movement, and player-control/package behavior. The full
sweep rejects unknown direct calls and captures VM error logs as failures.

This 100% figure means executable fragment coverage only. Each function is
currently started in an isolated modeled world, so it does not yet prove the
complete chronological MQ101 state machine, alias fill results, multiplayer
barriers, or parity of conservative native shims.

## Remaining hard boundary

Pixel rendering, audio mixing, exact Havok integration, navmesh path selection,
animation graph blending, face/lip animation, and undocumented native timing
will remain in-engine validation targets. Their logical inputs, ownership,
completion events, and replicated outputs can still be simulated and fuzzed.
