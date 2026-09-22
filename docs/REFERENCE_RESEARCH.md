# Reference research

## Runtime display switching and cursor behavior

- [SSE Display Tweaks](https://github.com/SlavicPotato/SSEDisplayTweaks)
  - Relevant files: `SSETweaks/window.cpp`, `SSETweaks/render.cpp`, and `SSEDisplayTweaks.ini`.
  - Its borderless-upscale path enlarges the native window while reporting a stable render client rectangle during D3D setup. This avoids treating every window-size change as a new Skyrim render resolution.
  - Its cursor lock is focus-aware: it applies `ClipCursor` on focus/activation and releases it on `WM_KILLFOCUS`, `WM_DESTROY`, or inactive `WM_ACTIVATE`.
  - Direction for this project: use a stable internal render size when toggling to borderless fullscreen and scale presentation to the monitor. Do not force Skyrim's full render-target rebuild merely to enlarge the window.

- [CommonLibSSE-NG `GViewport`](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/G/GViewport.cpp)
  - Confirms Skyrim initializes Scaleform viewport `scale` and `aspectRatio` to `1.0`; changing those fields to the physical monitor aspect is not a supported fix for the stretched 3D menu model.

- [Autodesk Scaleform `Viewport`](https://help.autodesk.com/cloudhelp/ENU/Scaleform-Help/cpp_ref/06771.html)
  - Scaleform's 3D presentation also has view/projection matrices. Updating only the 2D viewport can therefore resize menu text while leaving a stale 3D projection.

- [32:9 Skyrim interface notes](https://www.nexusmods.com/skyrim/mods/98700)
  - Existing 32:9 work treats the main-menu logo separately from the Flash UI and recommends the engine settings `fUIMistMenu_CameraFOV_G` and `fUIAltLogoModel_TranslateX_G` for its camera/model placement.
  - Direction for this project: measure these values and the live aspect ratio across F11 before changing them; do not assume the main-menu model is governed by the Scaleform viewport.

- [Skyrim Together widescreen compatibility report](https://github.com/tiltedphoques/Mod-Compatibility/issues/20)
  - Records that a dedicated widescreen interface replacement is compatible with Skyrim Together Reborn. It supports using established aspect-specific UI assets rather than treating 32:9 as only a swap-chain problem.

### Current conclusion

The observed 5120x1440 overlay and correctly resized text prove that the overlay texture and 2D viewport are updating. The still-stretched main-menu model is a separate Skyrim/Scaleform 3D projection problem. The next display implementation should follow SSE Display Tweaks' stable-render-resolution borderless upscale design instead of adding unverified `GViewport` field changes.

## Installed UI and engine research

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)
  - Local source snapshot: commit `b93280e832f263dbef44e44cbe2936622a02f91a`.
  - `MistMenu.h` documents the native 3D title/load-screen menu, including its
    model, camera path, shader, FOV, and rotation state.
  - `UI3DSceneManager.h/.cpp` documents the UI camera/frustum and the relocated
    `SetCameraFOV` function.
  - `BSScaleformManager.cpp` confirms that Skyrim initializes each loaded movie
    with the collected display size, safe rectangle, scale mode, and viewport.

- [SKSE64](https://github.com/ianpatt/skse64)
  - Local source snapshot: commit `25b72352adb6543fa6d0bd3795780672b2e238e0`.
  - Used as a second source for native UI/Scaleform behavior and plugin/runtime
    integration.

- [SkyUI](https://github.com/schlangster/skyui)
  - Local source snapshot: commit `835428728e2305865e220fdfc99d791434955eb1`.
  - Confirms the established ActionScript/SKSE extension surface and provides a
    source-quality reference for menu input and hot-reload development.

- [SSE Display Tweaks](https://github.com/SlavicPotato/SSEDisplayTweaks)
  - Local source snapshot: commit `41668a7497872366cb663d8c353f07e1001fa1c1`.
  - Its source identifies the exact window activation, cursor clipping,
    swap-chain resize, and borderless-upscale paths relevant to F11 behavior.

- [SKSE official builds](https://skse.silverlock.org/)
  - Confirms SKSE 2.3.1 targets Skyrim runtime 1.7.104.

- [Skyrim MCP](https://github.com/jarvann/SkryimMCM)
  - Local source snapshot: commit `82a0a80d31ae7d2254d40c129650d1e260da38a2`.
  - Demonstrates the same broad live-debug architecture we had proposed: an
    in-process native plugin, newline-delimited JSON over a Windows named pipe,
    and an out-of-process MCP server.
  - Its current UI manager only reports a fixed list of open menus. Our UI
    state recorder is substantially deeper, but its task-queue and named-pipe
    separation are useful references for the future general engine bridge.
  - The README declares MIT, although the inspected snapshot does not contain a
    standalone `LICENSE` file. Treat it as an architectural reference unless
    licensing is clarified before copying implementation code.

See `docs/UI_ARCHITECTURE.md` for the installed-asset inventory and the verified
five-layer main-menu model.

## CEF off-screen popup rendering

- [CEF `CefRenderHandler` documentation](https://cef-builds.spotifycdn.com/docs/147.0/classCefRenderHandler.html)
  - `OnPaint` distinguishes the main view from a popup widget, while
    `OnPopupShow` and `OnPopupSize` supply the popup's visibility and rectangle.
  - The current TiltedUI render handler accepts only `PET_VIEW`, so native HTML
    `select` menus are not composited into the overlay texture.
  - Direction for this project: use the existing in-page dropdown component for
    game settings now. If arbitrary web content is later required, implement
    the complete CEF OSR popup compositing contract instead of partially
    rendering popup widgets.

## Windows Snap and live window resizing

- [Microsoft: Support snap layouts for desktop apps on Windows 11](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-snap-layout-menu)
  - Standard Win32 caption/maximize behavior supplies Win+Z Snap Layouts. Apps
    must accept the selected zone size rather than immediately enforcing a
    previously stored size.
  - Microsoft recommends supporting a minimum width no larger than 500
    effective pixels; this project accepts windowed client sizes down to 330.

- [Microsoft: DXGI overview and handling window resizing](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
  - `WM_SIZE` is the authoritative notification for a resized HWND. The
    application should rebuild its swap-chain buffers/render targets for the
    new client area rather than moving the HWND back to an old resolution.

- [SSE Display Tweaks `window.cpp`](https://github.com/SlavicPotato/SSEDisplayTweaks/blob/master/SSETweaks/window.cpp)
  - Uses normal Win32 window messages and only calls `SetWindowPos` for an
    explicit centering/fullscreen operation. It does not continuously enforce
    the configured dimensions after an external window resize.

Direction for this project: distinguish programmatic display-mode transitions
from external `WM_SIZE` events. A Windows Snap operation becomes the new
windowed client size, cancels any stale display preview, and is persisted rather
than reverted.

The same Snap workflow also requires returning ownership of the shared Windows
pointer while the shell UI is open:

- [Microsoft `ShowCursor`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showcursor)
  - Cursor visibility uses a counter, so a single show call does not reliably
    undo a game's earlier hide calls; increment until the result is nonnegative.
- [Microsoft `ClipCursor`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor)
  - Microsoft explicitly requires an application to release cursor confinement
    before relinquishing control to another application.

Direction for this project: on either Windows-key press, release capture and
clipping, normalize `ShowCursor(TRUE)`, and keep the system arrow active for the
Snap chooser. Reclaim it only when focus or a click returns to Skyrim.

## Main-menu UI3D projection refresh

- [CommonLibSSE-NG `UI3DSceneManager.cpp`](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/U/UI3DSceneManager.cpp)
  - Maps the AE singleton to Address Library ID `403560` and Skyrim's supported
    `SetCameraFOV` method to ID `52742`.
  - Reapplying the live Mist Menu FOV through this method is preferable to
    manually writing `NiFrustum` fields because Skyrim recalculates the camera
    projection through its own engine path.

- [Skyrim Ultrawide RaceMenu Fix](https://github.com/theosw/SkyrimUltrawideRaceMenuFix)
  - Independently demonstrates that Skyrim menu-camera corrections must use
    the live `NiCamera` frustum/aspect rather than trusting configured INI
    resolution alone.

Direction for this project: after a renderer target rebuild or accepted Win32
resize, reapply `MistMenu::cameraFOV` using `UI3DSceneManager::SetCameraFOV` so
the title model cannot retain the preceding window's aspect projection.

## Complete live Scaleform reinitialization after resize

- [CommonLibSSE-NG `BSScaleformManager.cpp`](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/B/BSScaleformManager.cpp)
  - `LoadMovie` initializes every movie from `BSGraphics::State`, then applies
    its scale mode, safe rectangle, full framebuffer viewport, advances frame
    zero, and calls `IMenu::RefreshPlatform`.
- [CommonLibSSE-NG `GFxMovieView.h`](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/include/RE/G/GFxMovieView.h)
  - Confirms `SetViewport`, `SetViewScaleMode`, and `NotifyMouseState` are
    per-movie state. Updating Main Menu alone cannot resize the independent
    Cursor Menu movie.
Direction for this project: a runtime resolution transition synchronizes
`BSGraphics::State`, reapplies the viewport and existing scale mode to every
live Scaleform movie, calls each changed menu's `RefreshPlatform`, and then
reapplies the Mist Menu camera FOV. This mirrors the relevant
parts of movie startup and prevents Main Menu, Cursor Menu, and UI3D from each
retaining a different prior resolution.

The extracted 1.7.104 `startmenu.swf` ActionScript and Muse's consolidated
`docs/reverse-engineering/UI_SCALEFORM_MIST.md` add an important constraint:
`StartMenu::InitExtensions()` runs once, locks the menu root bottom-right and
`Logo_mc` bottom-left using `Stage.visibleRect`/`Stage.safeRect`, and has no
resize listener. A viewport update therefore cannot reproduce clean-start
widescreen placement. A live experiment that queued native Hide/Show messages
for Mist Menu and Main Menu recreated the shells but did not replay the native
callbacks that populate the menu, leaving it blank; that approach was rejected.
The selected direction is a resize entry point inside the patched SWF that
reflows the existing clips without destroying menu state. The separate
`MenuCursor` singleton (CommonLibSSE-NG ID `403551`) is also resized because
replacing only Cursor Menu's movie viewport leaves its native clamp bounds at
the preceding window size.

Implemented direction: the derived `startmenu.swf` keeps the original
`MovieClip.Lock` behavior but moves the three vanilla anchor calls into an
idempotent `SkyrimSeamlessRefreshLayout` method. A persistent frame watcher
compares the current `Stage.visibleRect` dimensions to the last applied values
and invokes that method only after a real size change. This avoids undocumented
native `GFxMovieView::Invoke` offsets and retains the populated menu instance.
The installed SWF was exported again after compilation to verify that the
watcher and reflow method survived bytecode import.

## Live options behavior

- Skyrim's extracted `quest_journal` `SystemPage.as` sends `OptionChange` on
  each slider/toggle change and reserves `SaveSettings` for persistence when
  leaving the page. This establishes the native UX target: changes are live,
  while saving is a separate operation.
- [SSE Display Tweaks configuration](https://github.com/SlavicPotato/SSEDisplayTweaks/blob/master/SSETweaks/SSEDisplayTweaks.ini)
  documents that its game-setting adjustments are in-memory rather than INI
  writes, supporting the same separation between runtime values and persisted
  launch configuration.

Direction for this project: every control updates the game's live settings
collection immediately. Non-display changes are persisted immediately as well.
Display mode, monitor, and resolution become live immediately but retain a
15-second keep/revert transaction because an unsupported mode can obscure the
confirmation UI. Confirmation persists the already-live state and must not
perform a second renderer rebuild.

Runtime testing showed that INI-collection writes alone do not drive Skyrim's
cached audio mixer. CommonLibSSE-NG documents `BGSSoundCategory`'s
`BSISoundCategory` interface at `+0x30` and virtual slot 3 as
`SetCategoryVolume(float)`, which is used for footsteps, voice, music, and
effects. Executable tracing showed that master volume is not a sound category:
native `OptionChange` case 27 updates separate live audio-manager state. The
implementation therefore invokes that verified native case for master volume
instead of treating a default-object category as equivalent.

The 1.7.104 executable's `OptionChange` callback was independently traced at
RVA `0x9AB260` (Address Library ID `53310`). Switch case 13, the Brightness
entry after the 13 gameplay entries, maps the slider through Skyrim's gamma
minimum/maximum and writes the live gamma value at RVA `0x20D0348` (Address
Library ID `388988`). The implementation writes ID `388988` explicitly after
updating `fGamma:Display`, reproducing the renderer-facing part of the native
callback without fabricating Scaleform callback arguments.

Microsoft's XAudio2 documentation establishes a separate constraint for output
device switching: the mastering voice represents the output endpoint, only one
mastering voice may exist per XAudio2 instance, and it cannot be destroyed while
source or submix voices remain. Therefore an output-device dropdown must use a
verified Skyrim audio-subsystem teardown/rebuild or process-route migration; it
must not merely store a device name and claim a live switch.

## Controller navigation for the CEF options menu

- [Microsoft: Getting Started With XInput](https://learn.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput)
  confirms that `XInputGetState` exposes the digital D-pad, sticks, and face
  buttons directly, independent of a game's higher-level menu bindings.
- [CommonLibSSE-NG `InputMap.cpp`](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/SKSE/InputMap.cpp)
  confirms Skyrim's gamepad input is represented by the same XInput button
  masks, including the four D-pad directions.
- [Xinput-Modkey-SE](https://github.com/ike9000e/Xinput-Modkey-SE) is prior
  open-source Skyrim SE work that polls XInput and translates controller state
  into keyboard-style inputs without replacing Skyrim's normal configuration.

Direction for this project: while the modal CEF overlay owns input, poll the
first connected XInput controller and translate D-pad/stick navigation plus
A/B into CEF keyboard events. This avoids depending on Skyrim forwarding input
to a non-Scaleform browser. Dropdowns are made keyboard-focusable and handle
left/right plus confirm explicitly; visible focus outlines make controller
position unambiguous. Skyrim's own device poll is suppressed for the lifetime
of the modal overlay so the translated press cannot also activate the native
main menu underneath it. Win32 focus loss, rather than the Windows-key event,
is the authoritative signal for releasing cursor clipping and restoring the
visible desktop cursor.

Follow-up runtime testing established that device ownership must be dynamic,
not controller-exclusive. D-pad up/down are delivered as arrow keys: the
settings panel uses them for focus movement, while an open dropdown consumes
them to move its highlighted option and A confirms it. Physical raw-mouse
movement immediately restores the browser pointer, and subsequent controller
navigation hides it again. `WM_ACTIVATE/WA_INACTIVE` is included in desktop
cursor release because opening the Windows shell can deactivate Skyrim without
delivering `WM_KILLFOCUS` or `WM_ACTIVATEAPP(FALSE)` first.

The final Win32 correction treats `GetForegroundWindow() != SkyrimHwnd` as
authoritative during `WM_SETCURSOR` and returns `TRUE` through the outer
renderer hook. Returning zero after setting the arrow allowed Windows/Skyrim
to treat the message as unhandled and hide it again. An automated test now
deactivates the real game HWND, moves the pointer over its client area, and
confirms `CURSOR_SHOWING` with the standard arrow handle.

## Unattended in-game regression harness

- [AutoTest](https://github.com/Muriel-Salvan/AutoTest) is prior open-source
  Bethesda-game work that runs configured suites inside Skyrim SE, records
  status in JSON/logs, captures screenshots, and can exit the game when the
  run completes. Its Papyrus/location focus is not sufficient for renderer and
  native UI assertions, but its unattended run/status/artifact model is useful.
- [Papyrus Debug Server](https://github.com/joelday/papyrus-debug-server)
  exposes Skyrim/Fallout Papyrus debugging through WebSocket and the Debug
  Adapter Protocol. It is useful when a failure is inside script execution,
  but it cannot explain native renderer, actor-process, network-authority, or
  world-state divergence by itself. Keep it as a later script-level adapter;
  the client bridge remains the source for native and multiplayer state.
- [skyrim-headless-mods / skytest](https://github.com/myaiexp/skyrim-headless-mods)
  demonstrates a detached, drivable Skyrim session with screenshots, injected
  input, engine-state polling, a runtime probe, and replayable `.steps` files.
  Its current Linux/gamescope launcher is not directly portable to this Windows
  two-PC setup, but its command/trace probe and action-plus-observable-gate
  model are adopted here as native JSON snapshots, transition timelines, and
  semantic leader/follower comparisons.
- [TiltedConnect's own transport test](https://github.com/tiltedphoques/TiltedEvolution/blob/dev/Libraries/TiltedConnect/Code/tests/src/connect.cpp)
  proves that the production client/server transport can run without Skyrim,
  but its upstream test loops forever and exchanges only an arbitrary string.
  Direction for this project: use the same real transport with production STR
  authentication/message codecs, bounded timeouts, structured JSON results,
  and two clients asserting party and authority behavior.
- [Steam Matchmaking](https://partner.steamgames.com/doc/api/ISteamMatchmaking)
  documents that lobby membership and connection to the advertised game server
  are separate actions. Protocol tests therefore cover authentication/party
  convergence independently; Steam lobby invitation/relay coverage remains an
  integration tier requiring Steam clients and distinct accounts.
- [Microsoft Windows UI automation guidance](https://learn.microsoft.com/en-nz/windows/apps/dev-tools/winapp-cli/ui-automation)
  explicitly requires an unlocked interactive desktop for OS input injection
  and warns that successful injection does not prove the target received the
  input; screenshots or inspected state must verify the effect.
- [ViGEmBus](https://github.com/ViGEm/ViGEmBus) documents automated game input
  replay as a virtual-controller use case. The upstream project is retired and
  a kernel driver adds installation risk, so it is deferred until tests need
  to validate Windows' complete XInput discovery path.

Direction for this project: do not call the Skyrim client truly headless.
Run it unattended on an unlocked, GPU-backed interactive desktop. Add an
in-process newline-delimited JSON test endpoint and an external runner that
records every command, structured native/CEF state, screenshot, timing, log,
and crash artifact. Inject controller actions directly at the mod's controller
translation seam first; reserve a virtual XInput device for a smaller
end-to-end hardware-discovery suite. Each action must be followed by an
observable assertion rather than treating successful input injection as a
passing test.

Implemented: `Tools/InGameTests/Run-InGameTests.ps1` combines the native
local-only named pipe with CEF's existing localhost DevTools endpoint. The
first exercised suite records native/DOM state, full-game and CEF screenshots,
and an event trace while testing Options, dropdown navigation, device handoff,
desktop cursor behavior, and a two-way display-mode transition. Its first runs
found a deterministic framed-window growth bug: renderer outer dimensions were
being restored as client dimensions. Windowed `ReadSettings` now uses
`GetClientRect`, and the exercised suite passes with a five-second delayed
renderer/process health check.

The native bridge protocol now also samples game-thread state at 10 Hz rather
than scanning Skyrim's quest collection every rendered frame. It reports the
local actor lifecycle, cell/worldspace, package, controls, camera, open menus,
party identity, watched quest stages, and a bounded quest-event journal.
`Tools/diagnostics-mcp` can capture a correlated screenshot/state/server/error
issue bundle, record a transition timeline, and semantically compare leader and
follower snapshots. These probes are observational; arbitrary console or code
execution is intentionally not exposed through MCP.

## Seamless co-op interaction and synchronization target

- [Elden Ring Seamless Co-op project page](https://www.nexusmods.com/eldenring/mods/510)
  - Establishes the experience target: one persistent session through death and
    bosses, synchronized NPC dialogue/talk events and map state, shared world
    reset when resting, Steam matchmaking, and host-world catch-up for joining
    clients.
  - It also documents an important design split: some state is deliberately
    instanced per player, such as shop stock, while world progression is synced.
  - Direction for this project: copy the low-friction session semantics, not
    undocumented Elden Ring internals. Skyrim requires its own explicit rules
    for leader-owned NPC AI, dialogue leases, door barriers, quest transactions,
    item instance identity, and save projection.

- [TiltedEvolution](https://github.com/tiltedphoques/TiltedEvolution), pinned
  locally at upstream `dev` commit
  `fbf72883015dba9e26bd1539d3af9d33fa794116` (GPLv3, with separately noted
  components in its `LICENSE`).
  - Relevant source includes `CharacterService` actor ownership epochs,
    `QuestService`, dialogue/subtitle messages, player respawn, inventory/object
    synchronization, and animation graph experiments.
  - Direction for this project: retain useful protocol and reverse-engineering
    work, but replace opportunistic per-actor ownership and reflected quest
    events with explicit leader authority and session transactions where the
    product contract requires one shared result.

- [Reborn quest-progression rework](https://github.com/tiltedphoques/TiltedEvolution/pull/848)
  - Documents that Skyrim may deliver quest progression events asynchronously,
    after a scoped suppression guard has expired. Mirrored clients therefore
    echo quest updates and can execute stages repeatedly.
  - Its server-side short-lived deduplication and scene tracking are valuable
    evidence, but timing-window deduplication is not a durable transaction ID.
    The project direction is a leader-issued quest transaction with persistent
    idempotency identity and a snapshot recovery path.

- [SkyMP](https://github.com/skyrim-multiplayer/skymp), pinned locally at
  commit `f926944b18e3aed4bc3864ce668626c05ec2545f`.
  - Its server-side `WorldState` and `MpChangeForm` provide prior art for
    persistent authoritative references, inventories, actor death, timers,
    disabled/deleted state, and respawn.
  - It is not a drop-in campaign solution: the inspected `PapyrusQuest`
    implementation reports current quest-stage lookup as unimplemented, and
    its inventory source notes incomplete ExtraData parsing. Adopt the
    persistent-world pattern while closing those gaps explicitly.

The resulting product and acceptance contract is recorded in
`docs/SEAMLESS_COOP_EXPERIENCE_SPEC.md`.

## Cooperative bleedout and revival

- [Acheron-NG](https://github.com/Acook1e/Acheron-NG), inspected locally at
  commit `59f744634fa5df9d645ce0e337ed94ab89297dbb` (Apache-2.0).
  - `src/Acheron/Hooks/Hooks.cpp` demonstrates intercepting lethal direct,
    magic-effect, damage-over-time, fall, and physics damage before ordinary
    death is committed.
  - `src/Acheron/Defeat.cpp` demonstrates a guarded defeat/rescue lifecycle:
    set no-auto-recovery, play Skyrim's `BleedoutStart`/`BleedoutStop` idles,
    hold health at a small positive floor, pacify the actor, disable relevant
    player controls, and use atomic state transitions to reject duplicate
    rescue attempts.
  - Direction for this project: reuse the verified Skyrim seams and lifecycle
    lessons, but make eligibility, revive leases, timers, state transitions,
    inventory retention, and recovery server-authoritative. Do not adopt its
    single-player global state directly.

- Current TiltedEvolution `PlayerService::RunRespawnUpdates` watches
  `IsBleedingOut`, starts a five-second deadline, forces health to zero when
  necessary, calls `RespawnPlayer`, and then sends `PlayerRespawnRequest`.
  The server optionally removes gold and broadcasts `NotifyRespawn` because
  remote body state can otherwise remain corrupted.
  - This is useful confirmation that Skyrim's bleedout state is already the
    available interception point, but the local client currently decides and
    executes respawn before the server. Our target reverses that authority:
  lethal damage requests a server transition, and only a committed down,
  revive, death, or respawn message may change the player lifecycle.

## Vanilla quest and Helgen analysis

- [Champollion](https://github.com/Orvid/Champollion), official source checked
  out locally at tag `v1.1.2`, commit
  `fd3798cd9672c2968cbd81660641f7cbbadb13fb` (LGPL-3.0).
  - It is the open-source PEX-to-Papyrus decompiler used to build a private,
    local semantic corpus from the user's installed Skyrim scripts. Its stated
    goal is functionally equivalent PSC reconstruction, but decompiler output is
    still treated as analysis evidence and correlated against plugin records and
    runtime behavior before hooks are selected.
  - The installed `Skyrim - Misc.bsa` contains 14,026 PEX files. All were
    extracted and decompiled outside the repository; 50 MQ101-related scripts
    form the first focused review set. Bethesda source text is not redistributed.

- [xEdit](https://github.com/TES5Edit/TES5Edit) and its
  [QUST record definition](https://github.com/TES5Edit/meta/blob/master/UESPWiki/QUSTDef.wiki).
  - The QUST structure provides the missing binding between stage INDX entries,
    objectives, aliases, and VMAD-attached quest/alias/fragment scripts. This is
    required because a decompiled function named `Fragment_N` does not by itself
    establish which quest stage invokes it.
  - Direction for this project: derive a record-level quest graph from the
    installed masters and join it to static script findings. Do not hard-code a
    wiki stage number or infer one from decompiler function order.

- The installed MQ101 corpus confirms direct manipulation of the sole local
  player, including cart vehicle attachment, chargen/RaceMenu, controls,
  AI-driven state, forced movement, inventory/equipment, saves, scenes, aliases,
  and player-relative triggers.
  - Direction for this project: do not add extra physical players to the authored
    cart/execution scene. Only the host executes MQ101; followers receive its
    cinematic presentation, use isolated local character creation behind a
    ready barrier, and materialize only at a verified host-control boundary.
    The design is recorded in
    `docs/reverse-engineering/INTRO_MQ101_MULTIPLAYER.md`.

- [Mutagen](https://github.com/Mutagen-Modding/Mutagen), NuGet package
  `Mutagen.Bethesda.Skyrim` pinned at `0.54.4` (GPL-3.0).
  - Its strongly typed read-only Skyrim record API is used by
    `Tools/QuestAudit/QuestRecordAudit` to correlate QUST stages, VMAD fragment
    names, objectives, aliases, and linked SCEN records from the installed
    masters. This avoids brittle byte parsing and makes the whole-game audit
    reproducible.
  - Direction for this project: emit only derived metadata and our analysis;
    omit Bethesda dialogue/journal text and never modify the installed masters.

## Headless quest and multiplayer simulation

- [SkyrimReLive](https://github.com/danmeedev/skyrimReLive) includes a
  no-game echo-client mode for networking smoke tests. It supports the same
  architectural boundary used here: exercise transport and replicated state
  without claiming to emulate the Skyrim executable.
- [OpenPapyrus](https://github.com/fireundubh/OpenPapyrus) and the open-source
  [Papyrus compiler/PEX reader](https://github.com/rethesda/papyrus-compiler-v)
  provide useful grammar, bytecode, and static-analysis references.
  - Direction for this project: use PEX and decompiled scripts to derive a
    conservative effect graph. These projects are not a complete Skyrim
    Papyrus/native-function runtime, so unknown conditions and engine callbacks
    remain explicitly uncertain and require in-engine replay.
- No mature open-source replacement for Skyrim's complete Papyrus VM, native
  function environment, AI, Havok, and quest-condition evaluator was identified
  in this review. `docs/MODSIM_ARCHITECTURE.md` therefore separates exact
  protocol/record tests, conservative script exploration, and GPU-backed game
  tests instead of presenting a headless simulator as full engine parity.
- The repository's pinned `TiltedConnect` test client and production
  `SkyrimProtocolBot` were selected for the initial ModSim adapter so scenarios
  exercise the same transport and message factories as the shipped server.
  A separate mock networking stack was rejected because it could pass while
  production serialization, authentication, or disconnect handling was broken.
- [SkyMP's current Papyrus VM](https://github.com/skyrim-multiplayer/skymp/tree/f926944b18e3aed4bc3864ce668626c05ec2545f/papyrus-vm)
  is MIT-licensed and implements PEX reading, bytecode operations, objects,
  events, inheritance, synchronous and promised native calls, and VM stacks.
  It was imported at commit `f926944b18e3aed4bc3864ce668626c05ec2545f`
  with its license and attribution intact. `SkyrimPapyrusProbe` proves it can
  parse the installed 1.7.104 MQ101 PEX files; this moves ModSim beyond a
  decompiled-text effect scanner toward executable quest scripts.
  - The adopted VM now powers `SkyrimPapyrusSim`; all 159 MQ101 `Fragment_*`
    functions execute individually with missing calls and VM error logs treated
    as failures. Native shims derived from SkyMP server behavior remain clearly
    separated into modeled logical effects and conservative engine surrogates.
- [SkyMP libespm](https://github.com/skyrim-multiplayer/skymp/tree/f926944b18e3aed4bc3864ce668626c05ec2545f/libespm)
  is also MIT-licensed and is a candidate for lower-level record and condition
  extraction where Mutagen does not expose enough data.
- SkyMP's server native-function and condition implementations are AGPL-3.0.
  They are valuable behavior references, but were not copied in this initial
  import. Reusing them directly would require preserving the AGPL license and
  source-availability obligations for the combined server component.

## Durable shared-campaign persistence

- [SkyMP server configuration](https://github.com/skyrim-multiplayer/skymp/blob/main/docs/docs_server_configuration_reference.md)
  and the locally pinned MIT `viet` storage interfaces demonstrate a
  server-owned database boundary with replaceable database drivers and
  asynchronous batched upserts. This supports the core decision that shared
  world truth must outlive both clients and the server process. The current
  slice adopts the durable server boundary, but not SkyMP's whole ChangeForm
  schema: Skyrim SE Multiplayer needs ordered cross-domain transactions and a
  common checkpoint watermark for quests, lifecycle, inventory, references,
  and later save projection.
- [CommonLibSSE serialization example by Ryan McKenzie](https://gist.github.com/Ryan-rsm-McKenzie/b1c1e04f95f471c200834b6ac7ebd962)
  demonstrates SKSE co-save records and versioned save/load callbacks. This is
  the correct later location for small client projection metadata such as
  campaign ID, player ID, and last applied revision. It is deliberately not
  used as the canonical campaign database: a follower's missing or stale
  co-save must be recoverable from the server ledger.
- [Elden Ring Seamless Co-op FAQ](https://ersc-docs.github.io/faq/) documents
  host-based progression and warns that session event flags persist into each
  participant's local save. Its separate `.co2` profile informs the product
  behavior, while raw host-save copying is rejected for Skyrim because an ESS
  also contains player identity, Papyrus VM state, aliases, persistent handles,
  and load-order-dependent data.

Implemented direction: `Code/campaign/CampaignLedger.*` is a SQLite WAL-backed
campaign sequencer with a stable campaign UUID, authority epoch, monotonic
revision, unique transaction IDs, durable journal, and checkpoint watermark.
The server opens it before constructing gameplay services. Authentication now
advertises campaign identity/revision/epoch, and leader quest updates commit to
the ledger before modifying the in-memory quest log or broadcasting. Native
ESS writes remain intentionally out of scope until snapshot/reconnect and save
barrier protocols can prove that all clients are materializing the same
committed revision.
