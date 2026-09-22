# In-game regression harness

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
