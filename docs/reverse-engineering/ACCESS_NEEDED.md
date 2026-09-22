# Access needed

A workspace-policy probe (`SkyrimSE.exe` name search under the Steam install
dir) returned no results, so direct access to the installed game directory is
**treated as unavailable**. Work continues on the extracted corpus; nothing
below was modified.

To deepen runtime verification later, read-only access is needed to:

- `SkyrimSE.exe` (1.7.104.0) — hash confirmation, string/setting inventory.
- `Data/Skyrim - Interface.bsa` — already extracted to the analysis cache.
- `Data/Skyrim.esm`, `Update.esm` — FormID/layout references only.
- `versionlib-1-7-104-0.bin` (Address Library) — ID→offset resolution.
- SKSE `skse64_1_7_104.dll` / loader — plugin API surface actually loaded.
- Live save files (`My Games/Skyrim Special Edition/Saves/`) — format
  observation only, never overwritten by research tooling.
