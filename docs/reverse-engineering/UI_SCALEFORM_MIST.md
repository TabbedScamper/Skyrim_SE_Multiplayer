# UI / Scaleform / Mist menu — Verified (local)

Consolidates `docs/UI_ARCHITECTURE.md` and `docs/REFERENCE_RESEARCH.md`.
All claims here are observed against the installed 1.7.104 runtime unless
marked otherwise.

## Five-layer title screen

1. **Main Menu (`startmenu.swf`)** — 1280x720 @ 30 FPS. Owns list, focus,
   animations, save/load panels, confirmations, most AS→native calls.
   Root locked bottom-right, `Logo_mc` bottom-left via Scaleform extensions.
2. **Cursor Menu (`cursormenu.swf`)** — 640x480 @ 24 FPS. `_xmouse`/`_ymouse`
   drive `_root.mc_Cursor` directly: a distinct coordinate space from the
   start menu and the CEF overlay.
3. **Mist Menu** — native `IMenu`: title/load-screen 3D model, camera path,
   shader, FOV (verified +0x100), rotation, mist/load state. Projection does
   NOT derive from start-menu text layout.
4. **UI3DSceneManager** — UI camera, menu objects/lights, cached transform,
   frustum. Singleton ID 403560; `SetCameraFOV` ID 52742.
5. **Multiplayer CEF overlay + D3D presentation** — separate browser texture;
   texture dims, D3D viewport/scissor, swap-chain dims, Win32 client area can
   diverge during mode switches.

Failure-mode rule: text resizing correctly while the title model stays
stretched = stale 3D projection, not a 2D viewport bug. Fix = reapply
`MistMenu::cameraFOV` via `SetCameraFOV` after target rebuild/resize.

## Vanilla Start Menu contract (installed 1.7.104 AS)

- 19 native→Flash callbacks, 34 Flash→native ops; states include `Main`,
  `CharacterLoad`, `CharacterSelection`, `SaveLoad`, confirmations.
- Multiplayer override adds index 13 `OPTIONS` signaling
  `_root.SkyrimSeamlessOptionsRequested`; native polls/clears it (no delegate
  handler dependency).
- Whole-UI totals: 131 native→Flash callback names, 142 Flash→native names —
  expose as a structured catalog for the bridge, not one-off hooks.

## Menu stack / input routing

- `UI` singleton menu registry + per-menu instance flags/input context/movie
  observed via F9 (registered menus, flags, input context, movie viewports,
  scale mode, alignment, mouse state). `GStack` presence/absence: Reported,
  needs exhaustive header check.
- Camera settings present in exe: `fUIMistMenu_CameraFOV_G`,
  `fUIMistMenu_Camera{LookAt,Pos}{X,Y,Z}_G`, `fUIMistModel_RotateZ_G`,
  translations, logo rotate/pan/zoom/fade, `fUICamera{Near,Far}Distance`,
  `fUIAltLogoModel_Translate{X,Y,Z}_G`, `fUIMistMenu_DefaultLogoNIFScale`.
  F9 reads them with float+raw so a bad source FOV is distinguishable from a
  non-rebuilt projection.

## Resize behavior — direction of record

- Follow SSE Display Tweaks' stable-render-resolution borderless upscale;
  do not force full render-target rebuilds to enlarge the window; do not hand
  -edit `GViewport` scale/aspect (`1.0` init is confirmed; not a fix lever).
- Distinguish programmatic transitions from external `WM_SIZE` (Snap becomes
  the new persisted size; cancel stale previews).

## Co-op relevance

Menu stack state (which menus open, modal blocks on game progress) must be
part of session state: a client stuck in `CharacterLoad` while the leader
runs the world is a desync source. Bridge seam: UI state recorder →
structured menu-stack snapshot (Skyrim MCP's fixed-list reporter is the
negative example; ours is deeper).
