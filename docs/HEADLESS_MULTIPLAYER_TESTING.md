# Headless multiplayer testing

Skyrim engine behavior still requires a GPU-backed, unlocked desktop, but a
large part of the multiplayer contract can be tested without launching Skyrim.
The protocol harness runs two production `TiltedConnect` clients against the
real dedicated server and uses the real authentication and message codecs.

## ModSim scenario runner

The first ModSim production adapter wraps those clients in versioned,
machine-readable scenarios:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\Tools\ModSim\Invoke-ModSim.ps1
```

Pass one or more `-ScenarioPath` values to run a subset. With no paths, the
runner discovers every JSON scenario below `Tools/ModSim/scenarios`. It checks
the supported schema contract, starts an isolated server on port 12579, runs
the appropriate production protocol-bot mode, and writes a normalized trace
for every scenario beneath `Tools/ModSim/artifacts/modsim-*`.

The current adapters cover join, leader handoff, follower reconnect, and quest
authority. Network fault fields already exist in the schema, but the runner
rejects non-zero delay/drop/duplicate/reorder requests until the fault injector
is implemented. It never silently ignores a requested simulation feature.

Scenario definitions live in:

- `Tools/ModSim/schema/scenario-v1.schema.json`
- `Tools/ModSim/scenarios/session/*.json`
- `Tools/ModSim/scenarios/quest/*.json`

The model/engine confidence boundary and planned lifecycle, dialogue, door,
inventory, and quest-effect simulators are described in
`docs/MODSIM_ARCHITECTURE.md`.

The suite also includes an MIT-licensed SkyMP PEX reader/VM and a corpus probe:

```powershell
& "$env:LOCALAPPDATA\SkyrimSEMultiplayer\BuildTools\xmake-3.1.0\xmake\xmake.exe" build -y SkyrimPapyrusProbe
.\build\windows\x64\releasedbg\SkyrimPapyrusProbe.exe C:\path\to\script.pex
```

The probe emits JSON containing PEX classes, parents, function/instruction
counts, and all statically named Papyrus call sites. This provides a measurable
native-shim backlog before scripts are executed in the modeled world.

Run the executable MQ101 fragment sweep with:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\Tools\ModSim\Invoke-PapyrusSweep.ps1
```

`SkyrimPapyrusSim` executes the installed PEX in the imported VM, supplies a
modeled quest/player/reference world, records every modeled native effect, and
turns unknown direct calls or VM error logs into failures. The sweep currently
executes all 159 `Fragment_*` functions in `QF_MQ101_0003372B` individually.
This is executable coverage, not a claim that conservative camera, physics,
animation, AI, or save shims match Skyrim.

Build and run:

```powershell
& "$env:LOCALAPPDATA\SkyrimSEMultiplayer\BuildTools\xmake-3.1.0\xmake\xmake.exe" build SkyrimServerRunner SkyrimProtocolBot
powershell.exe -ExecutionPolicy Bypass -File .\Tools\InGameTests\Run-ProtocolScenarios.ps1
```

Current scenarios:

- `join`: authenticates two clients, creates one party, and requires both
  clients to converge on the same member count and leader.
- `leader-handoff`: disconnects the leader and requires the remaining member
  to converge on a one-member party with itself as the new leader.
- `follower-reconnect`: disconnects the follower, checks that the leader sees
  the departure, then authenticates a replacement client and requires it to
  autojoin the existing leader-owned party.
- `quest-authority-audit`: verifies leader quest propagation, then sends a
  conflicting quest stage from the follower. The server must reject it, even
  if a stale or modified follower bypasses the client-side suppression.

The runner starts an isolated server on port 12578, writes all output beneath a
timestamped `Tools/InGameTests/artifacts/protocol-*` directory, and does not use
or alter either player's saves. Use `-FailOnKnownGaps` in CI so any newly
documented authority gap becomes a hard failure.

The first audit exposed that follower-authored quest stages were accepted and
broadcast. The client now suppresses follower quest transmission and the server
independently rejects it. This protects network authority; preventing local
vanilla follower-side quest scripts from executing before the leader commit is
a separate engine-level interception test.

This protocol tier should grow to cover reconnect/catch-up, stale ownership
epochs, inventory identity, death/down/revive transitions, dialogue leases,
door barriers, and deterministic packet-delay/reordering. Engine-level effects
then use the existing two-PC in-game probe and compare the resulting snapshots.
