# Source catalog — prior art and references

Local snapshot commits are from `docs/REFERENCE_RESEARCH.md` (verified local).
Upstream branch/commit fields say `dev (SHA unpinned)` where not yet pinned.

## Engine / scripting reference libraries

- **CommonLibSSE-NG** — https://github.com/CharmedBaryon/CommonLibSSE-NG
  - Local snapshot: `b93280e832f263dbef44e44cbe2936622a02f91a`. License: MIT.
  - Relevant: `RE/M/MistMenu.h`, `RE/U/UI3DSceneManager.h/.cpp`,
    `RE/B/BSScaleformManager.cpp`, `RE/B/BSScript/*VirtualMachine*`,
    `RE/A/Actor.h`, `RE/T/TESObjectREFR.h`, `RE/M/Main.h`.
  - Runtime: SE/AE/VR via `REL::RelocationID`; 1.7.104 covered by Address
    Library, but each ID used must be individually confirmed.
  - Reuse: yes for headers/IDs in SKSE plugins (MIT, attribution).
- **SKSE64** — https://github.com/ianpatt/skse64
  - Local snapshot: `25b72352adb6543fa6d0bd3795780672b2e238e0`.
  - License: verify before reuse (no standard OSS license asserted here).
  - Relevant: plugin load/API surface, task delegates, Scaleform/input hooks.
  - Runtime: SKSE 2.3.1 targets Skyrim 1.7.104 (verified local).
  - Reuse: reference only until license clarified.
- **Address Library (versionlib)** — meh321's ID→offset database.
  - Local: `versionlib-1-7-104-0.bin` matches installed exe (verified local).
  - Rule: cite IDs, never raw offsets, unless the offset is demonstrated on
    1.7.104 in a live probe.

## Multiplayer prior art (canonical)

- **TiltedEvolution** (Skyrim Together Reborn) —
  https://github.com/tiltedphoques/TiltedEvolution, branch `dev` (SHA unpinned).
  - NOTE: `github.com/tiltedphoques/TiltedOnline` now returns 404; do not cite
    it as canonical. Forks (`Znny/TiltedEvolution`, `cmpayc/TiltedEvolutionVR`)
    exist but are not upstream.
  - License: verify before reuse (not asserted here).
  - Verified (upstream) this run: `Code/server/GameServer.cpp` (37,913 bytes),
    `Code/server/Services/CharacterService.h` (epoch ownership, transfer
    reasons incl. `LeaderAssignment`/`LeaderClaim`), `Code/server/Services/`
    per-domain services, `Code/client`, `Code/common` shared code.
  - Architecture: dedicated server (`SkyrimTogetherServer.exe`) + clients;
    server `World` = `entt::registry` + dispatcher with Player/Party/
    Character/Calendar/Quest/ScriptService context (reported, `World.h` fetch
    partially verified).
  - Reuse: architecture and protocol design are the primary model; code reuse
    pending license + version-compat check (STR targets its own supported
    runtime, not necessarily 1.7.104).

## UI / rendering / debug references

- **SkyUI** — https://github.com/schlangster/skyui, local `835428728e2305865e220fdfc99d791434955eb1`.
  License: verify. Relevant: ActionScript/SKSE extension surface, menu input.
- **SSE Display Tweaks** — https://github.com/SlavicPotato/SSEDisplayTweaks,
  local `41668a7497872366cb663d8c353f07e1001fa1c1`. License: believed
  copyleft (verify before reuse). Relevant: `SSETweaks/window.cpp` (focus-aware
  cursor clip, no size enforcement after external `WM_SIZE`), `render.cpp`
  (stable-render-resolution borderless upscale).
- **Skyrim MCP** — https://github.com/jarvann/SkryimMCM, local
  `82a0a80d31ae7d2254d40c129650d1e260da38a2`. README declares MIT (no
  standalone LICENSE in snapshot — clarify before copying). Relevant:
  in-process native plugin + newline-delimited JSON over named pipe +
  out-of-process server; the task-queue/pipe separation is the model for our
  live debug bridge.
- **Skyrim Ultrawide RaceMenu Fix** — https://github.com/theosw/SkyrimUltrawideRaceMenuFix.
  Relevant: menu-camera fixes must use the live `NiCamera` frustum/aspect.
- **Nexus 32:9 interface notes** (mod 98700) and **STR widescreen report**
  (Mod-Compatibility #20): aspect-specific UI assets are established practice.

## Docs / specs

- Autodesk Scaleform `Viewport` C++ ref (view/projection separate from 2D
  viewport); CEF `CefRenderHandler` OSR popup contract; MS DXGI resize and
  Win11 Snap guidance — see `docs/REFERENCE_RESEARCH.md`.
