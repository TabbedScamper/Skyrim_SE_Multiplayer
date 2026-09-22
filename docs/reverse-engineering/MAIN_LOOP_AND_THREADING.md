# Main loop and threading

## Papyrus VM update — Reported

- `BSScript::Internal::VirtualMachine` exposes `Update` (slot 04),
  `UpdateTasklets` (slot 05), `SetOverstressed`, `IsCompletelyFrozen`, and a
  `FreezeState` enum (upstream header, unpinned branch).
- Implication: frame script load is budgeted; a leader-driven session must
  watch overstress/freeze state because stalled VMs stall quest/script
  replication. Live-probe the overstress flag under load (P3).
- `fUpdateBudgetMS` / `fExtraTaskletBudgetMS` = 1.2 ms defaults rest on
  secondary sources (primary wiki fetch 403'd) — Hypothesis.

## Main update — Hypothesis

- `Main::Update` RelocationID (35551, 36544) + offsets 0x11F/0x160 are
  unpinned and single-sourced; VR ID missing. Candidate safe-mutation point
  only after re-verification.
- Preferred mutation points until then (in order): SKSE task delegates /
  `BSTaskPool` (reported, unverified) → Papyrus tasklets → render-callback
  read-only observation. Never mutate game state on the render thread.

## Render vs game thread — Verified (local, partial)

- The CEF overlay texture, D3D viewport/scissor, swap-chain dims, and Win32
  client area can all diverge during a live mode switch (observed via F9).
  Any hook that reads presentation state must state which layer it read.

## Pause / focus — Verified (local, partial)

- SSE Display Tweaks model (local snapshot `41668a7`): cursor lock is
  focus-aware (`ClipCursor` on activation, release on `WM_KILLFOCUS` /
  `WM_DESTROY` / inactive `WM_ACTIVATE`); no continuous size enforcement
  after external `WM_SIZE`.
- Engine pause-on-focus-loss and VM behavior while unfocused: Unknown — probe
  P4. A leader-authoritative session must define whether alt-tab pauses
  simulation for everyone or only the unfocused client.

## Safe mutation points (current ordering)

1. SKSE main-thread task delegation (to verify). 2. Papyrus latent/tasklet
   context (to verify). 3. `UI3DSceneManager::SetCameraFOV`-style engine
   functions (verified pattern for camera). 4. Direct field writes — last
   resort, probe-gated.
