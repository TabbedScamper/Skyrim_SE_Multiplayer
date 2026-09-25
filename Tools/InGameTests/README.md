# In-game regression harness

## Two-PC authority comparison

With both games running and connected, capture a read-only convergence report:

```powershell
.\Tools\InGameTests\Compare-TwoPcAuthority.ps1
```

The report compares campaign/authority epochs, loading presentation, camera
scene-node transforms, animation/pose diagnostics, and nearby dynamic
reference transforms. It writes the full result beneath `artifacts/`.
It samples both clients at the same shared world tick (within 200 ms), and
fails rather than silently comparing out-of-window observations. Scene
comparisons include guarded, read-only phase-eligible action metadata.

`Deploy-TwoPcClient.ps1 -Launch` installs the client, server DLL, and
`GameTestKeyHelper.exe` with matching SHA-256 hashes on both PCs.
`Start-TwoPcNewCampaignFast.ps1` waits for the host lobby, verifies the
follower joined, then checks readiness and the shared start epoch; accepting
a pipe command is not treated as proof of launch. The test-only
`race_menu_key` bridge accepts `done` and `confirm` only while native
`RaceSex Menu` is visible on an unlocked interactive desktop. `done` should
open `MessageBoxMenu`, but helper exit 0 only means the key was sent; verify
the transition with `race_menu_state`. The separate
helper exists because this runtime ignored equivalent keys synthesized from
inside the Skyrim process.

For the vanilla English "Finish and name your character?" confirmation, the
test-only `confirm_character_native` command queues the native message-box
selection on Skyrim's game thread. Poll `confirm_character_native_status` and
then `race_menu_state`: `nativeSelected=true` means the native callback was
invoked, while `messageBoxOpen=false` verifies the visible transition. The
name-entry field can then be driven with `race_menu_key type` and
`race_menu_key accept_name`. The native path is deliberately restricted to
the single expected character-creation dialog; it is not a general message-box
automation API. `Invoke-TwoPcTestBridge.ps1 -RequestJson '{"id":1,"command":"race_menu_state"}'`
queries both installed clients from the repository PowerShell session.

`create_test_checkpoint` is disabled. A paired attempt to call Skyrim's
`Save_Impl` directly during the intro hung both clients and left a zero-byte
`.ess.tmp` on the host. After both clients reach `party_state.sessionState=3`,
`gameplay_key` with `{"key":"quicksave"}` can request Skyrim's normal F5 path.
The paired stage-160 F5 saves in
`artifacts/checkpoint-stage160-20260924-024952/` were subsequently reloaded
through shared Continue on both PCs, returning both to MQ101 stage 160 with
unblocked controls and remote player render entities. They are separate local
save files, not a host-save copy or a synchronized transaction; a new save
still requires file and reload validation. `Start-TwoPcNewCampaignFast.ps1
-CampaignMode continue` uses each machine's latest local save for fast
post-dragon regression tests. `Advance-TwoPcCharacterCreator.ps1` automates
the native creator only when both games reach it; a later fresh run stalled
at MQ101 stage 41 and the helper correctly timed out without skipping stages.

For a bounded two-PC animation/pose capture during a live campaign, run
`Capture-TwoPcPose.ps1`. It arms one read-only bone/ragdoll sample on both PCs
at a shared world tick, waits for both samples, then compares the matching
snapshots and writes an `authority-*.json` artifact. The expensive full pose
walk is off for all other frames. `Set-TwoPcPoseProbe.ps1` only arms a sample;
it no longer enables continuous scanning. The game retains the last one-shot
sample for `game_pose_snapshot`, so slow frames do not evict it from the normal
rolling snapshot history. The comparison still rejects pose samples more than
200 shared-tick milliseconds apart.

This harness runs Skyrim SE Multiplayer on an unlocked, GPU-backed Windows
desktop. It is unattended, but intentionally not truly headless: Skyrim's
DXGI, focus, cursor, Scaleform, CEF, audio, and XInput behavior depend on an
interactive desktop.

Run:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\Tools\InGameTests\Run-InGameTests.ps1
```

Use `-NoLaunch` to attach to an already running build. Each run creates a
timestamped directory under `artifacts` containing native and DOM snapshots,
full-game BMP captures, CEF PNG captures, the command/event trace, and a JSON
pass/fail summary.

Each checkpoint also captures a game-thread snapshot and a paired BMP plus
`.game.json` bundle. The snapshot includes player death/bleedout/combat and
cell state, current AI package, individual control handlers, camera, open menu
stack, party authority, watched quest state, and the recent local/remote quest
event journal. MQ101 is watched by default; use the native `watch_quest`
command or the diagnostics MCP tool to add other editor IDs.

Native commands use the local-only named pipe
`\\.\pipe\SkyrimSEMultiplayer.Test`. Browser inspection uses CEF's existing
localhost DevTools endpoint on port 8384. The current smoke suite covers:

- Options opening and visibility;
- controller focus, dropdown navigation, and confirmation;
- controller-to-mouse and mouse-to-controller cursor handoff;
- Windows-key focus loss and cursor visibility over the unfocused game;
- F11-equivalent window-mode round trips and exact state restoration;
- structured window, renderer, swap-chain, menu, overlay, and cursor state;
- screenshots and replayable event traces at each checkpoint.

The workstation must remain logged in and unlocked. OS input injection cannot
operate on the secure or locked desktop.

## Physical two-PC protocol test

`Run-TwoPcProtocolTest.ps1` runs without Skyrim or an interactive desktop. It
starts the real server on the primary PC, runs one production-protocol bot on
each physical rig, and requires both machines to converge on the same party,
ready barrier, session state, and nonzero start epoch. With `-EnableModCheck`,
both clients also authenticate with the same synthetic content fingerprint.

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\Tools\InGameTests\Run-TwoPcProtocolTest.ps1 -EnableModCheck
```

The second PC must be reachable through the LAN-only SSH pairing. This test
validates the real cross-machine transport and protocol, but not Steam overlay,
Scaleform, rendering, input, audio, physics, or Skyrim engine behavior.
