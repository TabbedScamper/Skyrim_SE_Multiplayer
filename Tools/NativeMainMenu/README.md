# Native Skyrim main-menu integration

`Patch-NativeMainMenu.ps1` derives a patched `interface\startmenu.swf` from the
user's own installed copy of Skyrim. It adds selectable `CO-OP` and `OPTIONS`
entries. `CO-OP` opens the title-screen shared-campaign lobby; `OPTIONS` opens
the multiplayer client's live settings overlay. It
also adds a lightweight `Stage.visibleRect` watcher to the existing Start Menu
instance. When the renderer changes size, the watcher reruns Skyrim's original
bottom-right, bottom-left, and top-left anchor operations without closing or
recreating the menu.

The repository does not contain or redistribute Bethesda's original SWF.

Run from an elevated PowerShell prompt when Skyrim is closed:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
& .\Tools\NativeMainMenu\Patch-NativeMainMenu.ps1
```

The script validates the expected ActionScript structure before it writes to the
game directory and backs up any existing loose `startmenu.swf` first. It uses
BSArch from xEdit and JPEXS Free Flash Decompiler, downloading fixed versions
from their official GitHub releases into the current user's local app-data cache.
