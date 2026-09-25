# SkyrimSE.exe 1.7.104: the whole executable, decompiled and queryable

Built 2026-09-24 by Reviewer A. Supersedes `ACCESS_NEEDED.md`: the exe **is** readable.

## What exists

`C:\Tools\skyrim_re\` (outside the repo; nothing Bethesda-derived is committed here):

| Path | What |
|---|---|
| `ghidra\Skyrim_1_7_104` | Ghidra 12.1.2 project, full auto-analysis, RTTI, names applied |
| `corpus\shard_NN.c` + `index.csv` | decompiled C for all **176,095** functions (3 failed) |
| `graph\functions.tsv` | rva, size, name, prototype |
| `graph\calls.tsv` | caller -> callee edges with callsite |
| `graph\strings.tsv` | every string + the functions that reference it |
| `graph\vtables.tsv` | every RTTI vftable, slot -> function |
| `graph\papyrus_natives.tsv` | 637 Papyrus natives (class, name, impl VA or `-`, binder). **Partial**: 203 have a direct impl VA |
| `id_to_rva.tsv` | every Address Library ID -> RVA for 1.7.104 |
| `names.tsv` | 15,765 names: CommonLibSSE-NG + our own `POINTER_SKYRIMSE` sites (`STR_<file>_<var>`) + `Papyrus_<Class>_<Fn>` |

Facts established while building it:
- `SkyrimSE.exe` is wrapped by SteamStub (`.bind` section, entry point inside it), but `.text` is **not encrypted**
  (47k standard prologues, entropy 6.2). Static analysis of the installed file needs no DRM removal. Work only on
  the copy in `C:\Tools\skyrim_re`; never modify the install.
- SHA-256 prefix `846efccf0c1374d7`, which matches `RUNTIME_1_7_104.md`.

## How to query (no Ghidra launch per question)

```
C:\Tools\skyrim_re\sk.cmd id 38533                     # Address Library ID -> address + function
C:\Tools\skyrim_re\sk.cmd find "ControlMap"             # functions by name
C:\Tools\skyrim_re\sk.cmd fn Papyrus_Game_DisablePlayerControls   # decompiled C
C:\Tools\skyrim_re\sk.cmd fn 0x140693200               # by VA (also rva:xxxx, id:NNNN)
C:\Tools\skyrim_re\sk.cmd callers ControlMap::ToggleControls
C:\Tools\skyrim_re\sk.cmd callees Actor::SetLifeState
C:\Tools\skyrim_re\sk.cmd str "You are bleeding out"   # string -> the code that uses it
C:\Tools\skyrim_re\sk.cmd grep "ToggleControls\(singleton" 20   # regex over all decompiled C
C:\Tools\skyrim_re\sk.cmd vt "^FirstPersonState$"      # vtable slots
```

A name matching several functions prints the candidates. Pass the VA to disambiguate.
Rebuild steps: `scripts\build_names.py` -> Ghidra `SkyrimApplyNames.java` -> `post_analysis.ps1` -> `scripts\papyrus_natives.py`.

## Rule: read the code before adding a probe

When a live measurement doesn't settle a question after two tries, or a struct offset or flag meaning is being
inferred from observed values, stop measuring and read the function in the corpus. It takes seconds.
Live probes are for **confirming** what the code says, not for discovering it.

## Worked example (answered in minutes today, after ~15 h of probing)

Question: why could the host not move after respawn, when our snapshot said "movement enabled"?

1. `sk.cmd fn Papyrus_Game_DisablePlayerControls` shows the native only builds a flag mask and calls
   `ControlMap::ToggleControls(singleton, mask, enable=0, alsoStored=1)`. abMovement contributes `0x401`
   (CommonLib UEFlag: Movement 0x1 | Jumping 0x400). It optionally also calls `PlayerControls::SetEnabled(0)`.
2. `sk.cmd fn ControlMap::ToggleControls`: the live lock bits are **`ControlMap+0x120`**
   (`enabledControls`). `ControlMap+0x124` is a stored snapshot, where `0x80000000` means none is stored.
   `LoadStoredControls` copies +0x124 into +0x120.
3. **Our client mislabels this.** `PlayerControls.cpp` calls ID 400863 `BSInputEnableManager::s_instance` and
   ID 68545 `BSInputEnableManager::EnableOtherEvent`. The corpus shows 400863 is the **ControlMap singleton** and
   68545 is **`ControlMap::ToggleControls`**. Our `controls.movement=true` read comes from PlayerControls
   handlers, which is a different layer, so it could never see this lock.
4. `sk.cmd callers ControlMap::ToggleControls` lists 13 callers, including **`Actor::SetLifeState`**. For the
   player it does:
   - entering bleedout: `StartBleedoutCamera` (camera state 11), then `ControlMap::StoreControls`, then
     `ToggleControls(0x424, disable)`, which is Jumping | POVSwitch | Activate, not Movement
   - leaving bleedout: `StopBleedoutCamera`, which restores the camera state held at PlayerCamera+0xE8, then
     `LoadStoredControls`

   So any exit from bleedout that skips `SetLifeState` also skips restoring the camera and the controls. Any
   Papyrus `DisablePlayerControls` issued during bleedout is also written into the stored snapshot
   (alsoStored=1), and `LoadStoredControls` re-applies it on recovery.

Next live step (confirming, not discovering): add `ControlMap+0x120/+0x124` to `game_snapshot`. Then decode the bits
on a stuck player. A missing `0x1` means a Movement lock; `0x401` points to Papyrus DisablePlayerControls;
`0x424` only means the bleedout toggle was never restored. Then decide which caller did it.

## Known gaps (improve these, don't rediscover them)

- Most functions are still `FUN_`: only 1,285 are named. RTTI names every vtable, so use `vt` to name methods.
  Types are the real bottleneck, not the decompiler. Importing the skyrim-re-toolkit `.gdt`
  (`C:\Users\mwalt\SkyrimResearch\skyrim-re-toolkit`, release gdt-2026-08-29, AE archive) into the project would
  replace most raw `param_1 + 0x120` offsets with field names.
- Papyrus natives decompile with wrong signatures (`in_stack_...`). Their real signature is
  `(VM*, StackID, <self or none>, args...)`, and applying it would fix the argument names.
- The Papyrus native map misses natives registered in other shapes. Extend `scripts\papyrus_natives.py`.
- Behavior-graph event names (`IdleWalkingCameraStart`) live in `.hkx` files, not the exe, so `str` won't find them.
