# Runtime 1.7.104 map

## Executable baseline — Verified (local)

- `SkyrimSE.exe` version **1.7.104.0**.
- SHA-256: `846EFCCF0C1374D71F892907F46549560F2FCB0A75CB87A3EED438BAA0F1402F`.
- Matching Address Library: `versionlib-1-7-104-0.bin`.
- SKSE 2.3.1 targets this runtime. Base UI: `Data/Skyrim - Interface.bsa`
  (415 files, 53 SWF + 6 GFX); only loose override observed:
  `Data/Interface/startmenu.swf`.
- Source: `docs/UI_ARCHITECTURE.md`.

## Known symbols / IDs

- UI3DSceneManager singleton: Address Library ID **403560** — Verified
  (local, via CommonLibSSE-NG snapshot `b93280e` + F9 report).
- `UI3DSceneManager::SetCameraFOV`: ID **52742** (AE family) — Verified
  (local). Prefer calling it over writing `NiFrustum` fields.
- Mist Menu camera FOV at **+0x100** object bytes — Verified (local, F9
  report); an alternate layout candidate is recorded in the report — treat any
  other Mist Menu field offset as candidate-only.
- Papyrus VM: `BSScript::Internal::VirtualMachine::Update` vtable slot **04**,
  `UpdateTasklets` slot **05**, plus `SetOverstressed` / `IsCompletelyFrozen` /
  `FreezeState` — Reported (upstream header fetch, branch unpinned). Confirm
  against the pinned CommonLibSSE-NG commit before hooking.
- `Main::Update`: RelocationID reported as (35551, 36544), offsets 0x11F/0x160
  — Reported, unpinned, VR ID missing. Do not use until re-verified.
- `Actor::IsInBleedout` ID 48461 (SE) — Reported, single-source. Needs
  SE/AE/VR triple check.
- `Actor::Kill` / resurrect vtable slots — Unknown; explicitly unresolved.

## Confidence rules

1. Address Library IDs are the only portable reference; raw offsets are
   candidates until a live 1.7.104 probe confirms them.
2. Every ID reused from CommonLibSSE-NG must name the commit it was read at.
3. `Main.h` raw fetch at the assumed CharmedBaryon path 404'd this run —
   re-resolve the path (powerof3 vs alandtse fork divergence) before citing
   `Main` layout.
