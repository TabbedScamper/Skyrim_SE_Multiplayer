# Engine Atlas: exact-runtime address candidates

`Build-FunctionManifest.py` combines the installed Address Library v5 table
with CommonLibSSE-NG's function/label declarations through the source-visible
[`skyrim-re-toolkit` miner](https://github.com/ByteBard97/skyrim-re-toolkit).
It reads the game EXE only to hash it. It does not patch, launch, or modify Skyrim.

The current tested source pins are toolkit `b5039b37de51c78a04d0319fbdb4c436a339853a`
and the 1.7-aware CommonLibSSE-NG fork `2dde70e8bdf9890bbd5e648966c7d2c24e83092f`.
The toolkit's older bundled CommonLib is not the preferred type/layout source for 1.7.104.
From the repository root on this workstation:

```powershell
$pythonExe = 'C:\Users\mwalt\AppData\Local\Programs\Python\Python312\python.exe'
$toolkit = 'C:\Users\mwalt\SkyrimResearch\skyrim-re-toolkit'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
& $pythonExe Tools\EngineAtlas\Build-FunctionManifest.py `
  --toolkit-scripts "$toolkit\type-importer\scripts" `
  --commonlib 'C:\Users\mwalt\SkyrimResearch\CommonLibSSE-NG-1.7' `
  --commonlib-commit 2dde70e8bdf9890bbd5e648966c7d2c24e83092f `
  --addrlib "$game\Data\SKSE\Plugins\versionlib-1-7-104-0.bin" `
  --exe "$game\SkyrimSE.exe" `
  --out 'C:\Users\mwalt\SkyrimResearch\symbols-1-7-104-modern-commonlib.json'
```

Inspect whether candidate RVAs coincide with x64 unwind-function boundaries:

```powershell
& $pythonExe Tools\EngineAtlas\Inspect-FunctionBoundary.py `
  --exe "$game\SkyrimSE.exe" `
  --manifest 'C:\Users\mwalt\SkyrimResearch\symbols-1-7-104-modern-commonlib.json' `
  --ids 36544 39484 62640 77851 78382
```

For a crash dump with a module-relative RVA, the same inspector accepts
`--rva 0x...` (without requiring an Address Library ID). To view at most
256 bytes of exact installed EXE instructions:

```powershell
& $pythonExe Tools\EngineAtlas\Disassemble-Rva.py --exe "$game\SkyrimSE.exe" --rva 0x11cc0a0 --bytes 96
```

Neither tool proves a function name or that the faulting function caused the
invalid argument.

`pdata-start` supports a function-start candidate; `no-pdata-entry` is
inconclusive because leaf functions and thunks may lack unwind metadata.
Neither status proves a name, call signature, receiver, or safe hook phase.

Run parser unit tests with:

```powershell
& $pythonExe -m unittest discover -s Tools\EngineAtlas -p 'test_*.py' -v
```

The JSON contains source name, Address Library ID, RVA, candidate kind,
provenance and ID-name ambiguity. **Only the ID-to-RVA lookup is mechanically
confirmed** against this Address Library file. Names, class layouts, ABI,
function boundaries and safe call phases require exact-EXE disassembly and
bounded live validation before use in a patch. The output intentionally stays
outside Git and contains no executable bytes or game assets.
