# Skyrim UI and main-menu architecture

This document records verified facts for the installed Skyrim runtime and keeps
version-specific observations separate from assumptions. Generated Bethesda
assets and decompiled ActionScript remain in the local analysis cache and are
not redistributed by this repository.

## Installed runtime baseline

- `SkyrimSE.exe` version: **1.7.104.0**
- Executable SHA-256:
  `846EFCCF0C1374D71F892907F46549560F2FCB0A75CB87A3EED438BAA0F1402F`
- Matching Address Library: `versionlib-1-7-104-0.bin`
- Base UI archive: `Data/Skyrim - Interface.bsa`
- Base UI archive contents: 415 files, including 53 SWFs and 6 GFX movies.
- The only loose base-menu override observed during the inventory is
  `Data/Interface/startmenu.swf`.

Address Library IDs remain the preferred way to locate engine functions and
singletons. Raw offsets must be treated as candidates until they are confirmed
against 1.7.104.

## Main-menu layers

The title screen is not one UI surface. It is at least five cooperating layers:

1. **Main Menu / `startmenu.swf`**
   - Flash stage: 1280x720 at 30 FPS.
   - Owns the menu list, focus, animations, save/load panels, confirmation
     states, and most calls from ActionScript into native Skyrim.
   - Locks its root to the bottom-right and `Logo_mc` to the bottom-left through
     Scaleform extensions.
2. **Cursor Menu / `cursormenu.swf`**
   - Flash stage: 640x480 at 24 FPS.
   - A mouse listener assigns `_xmouse` and `_ymouse` directly to
     `_root.mc_Cursor`.
   - This is a distinct coordinate system from the 1280x720 start menu and from
     the pixel-sized CEF overlay.
3. **Mist Menu**
   - Native `IMenu` containing the title/load-screen 3D model, camera path,
     shader property, camera FOV, camera rotation, and mist/load-screen state.
   - It does not derive its projection from `startmenu.swf` text layout.
4. **UI3DSceneManager**
   - Owns the UI camera, menu objects/lights, cached camera transform, and view
     frustum. Address Library ID 403560 locates its singleton and ID 52742
     locates `SetCameraFOV` on AE-family runtimes.
5. **Multiplayer CEF overlay and D3D presentation**
   - The project draws a separate browser texture over Skyrim's render target.
   - Its texture dimensions, the current D3D viewport/scissor, the swap-chain
     dimensions, and the Win32 client area can all diverge during a live mode
     switch.

This separation explains the observed failure modes: Scaleform text can resize
correctly while the Mist Menu model retains a stale projection; the CEF cursor
can be correct while the 640x480 Cursor Menu remains in a different coordinate
space; and a correctly resized swap chain can still be clipped by a stale D3D
viewport.

## Vanilla Start Menu interface

The installed vanilla 1.7.104 ActionScript registers 19 native-to-Flash
callbacks and invokes 34 named Flash-to-native operations. Important flows
include:

- Native populates the initial list through `sendMenuProperties`.
- ActionScript invokes `NEW`, `CONTINUE`, `PopulateCharacterList`, `LoadGame`,
  `QuitToDesktop`, and `PlaySound` through `gfx.io.GameDelegate`.
- State transitions use named states such as `Main`, `CharacterLoad`,
  `CharacterSelection`, `SaveLoad`, and confirmation states.
- The installed multiplayer override adds index 13, `OPTIONS`, and signals
  `_root.SkyrimSeamlessOptionsRequested`. Native code polls and clears that
  value, so opening options does not depend on Skyrim having a registered
  `SkyrimSeamlessOptions` delegate handler.

The whole extracted UI contains 131 unique native-to-Flash callback names and
142 unique Flash-to-native call names. A later bridge can expose these as a
structured catalog instead of adding one-off hooks.

## Main-menu camera controls found in SkyrimSE.exe

The 1.7.104 executable contains the following relevant setting names:

- `fUIMistMenu_CameraFOV_G`
- `fUIMistMenu_CameraLookAtX_G`, `Y`, and `Z`
- `fUIMistMenu_CameraX_G`, `Y`, and `Z`
- `fUIMistMenu_DefaultLogoNIFScale`
- `fUIAltLogoModel_TranslateX_G`, `Y`, and `Z`
- `fUICameraNearDistance` and `fUICameraFarDistance`
- `fUIMistModel_RotateZ_G` and the three mist-model translation settings
- logo mouse/thumbstick rotate, pan, zoom, fade, and auto-rotate settings

The F9 state report reads the most important values through Skyrim's setting
collection and records both their interpreted float and raw value. This lets us
distinguish a bad source FOV from a projection that was not rebuilt after a
resize.

## Diagnostic contract

F9 writes a timestamped report under the runtime's `debug-feedback` directory.
It records:

- Win32 window/client rectangles, styles, DPI, and foreground state;
- renderer fullscreen/borderless/resize fields;
- swap-chain and active render-target descriptions;
- all D3D11 viewports and scissors;
- every registered menu, its live instance, flags, input context, and movie;
- every live movie's Scaleform viewport, scale mode, alignment, and mouse state;
- the Mist Menu object bytes, the verified `+0x100` camera FOV, and an
  alternate layout candidate;
- the UI3DSceneManager bytes, camera pointer, cached transform, and frustum;
- relevant main-menu camera settings.

The report is deliberately observational. Any write to the Mist Menu or UI 3D
camera should be implemented through a known engine function where possible and
validated by comparing before/after reports.

## Local research corpus

The local cache under
`%LOCALAPPDATA%/SkyrimSEMultiplayer/AnalysisTools` contains:

- an extracted copy of `Skyrim - Interface.bsa` from this installation;
- locally decompiled ActionScript for the UI movies;
- fixed-version BSArch and FFDec tools;
- source snapshots of CommonLibSSE-NG, SKSE64, SSE Display Tweaks, and SkyUI.

These inputs are local research artifacts and are intentionally not committed.
