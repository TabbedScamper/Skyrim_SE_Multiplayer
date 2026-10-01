#!/usr/bin/env python3
"""Reject files that the starter cannot safely install."""

import runpy
import sys
from pathlib import Path

safe_path = runpy.run_path(str(Path(__file__).with_name("make-manifest.py")))["safe_path"]
root = Path(sys.argv[1])
seen = set()
for file in root.rglob("*"):
    if file.is_symlink():
        raise SystemExit(f"symlink in payload: {file}")
    if not file.is_file():
        continue
    relative = file.relative_to(root).as_posix()
    lower_name = file.name.lower()
    if (lower_name.endswith((".pdb", ".lib", ".exp", ".log"))
            or lower_name.endswith("tests.exe")
            or lower_name in {"gametestkeyhelper.exe", "inspect-minidump.exe", "skyrimprotocolbot.exe"}
            or lower_name.startswith("skyrimpapyrus") and lower_name.endswith(".exe")
            or any(part.lower() in {"tests", "snapshots", "harness"} for part in file.relative_to(root).parts)):
        raise SystemExit(f"non-release file in payload: {relative}")
    if not safe_path(relative) or relative.lower() in seen:
        raise SystemExit(f"invalid or duplicate Data path: {relative}")
    seen.add(relative.lower())
required = {
    "SkyrimTogetherReborn/SkyrimTogether.exe",
    "SkyrimTogetherReborn/Skyrim_SE_Multiplayer.exe",
    "SkyrimSEMultiplayer.esp",
    "SkyrimSEMultiplayerQuestPatches.esp",
}
if not {path.lower() for path in required}.issubset(seen):
    raise SystemExit("release payload is missing required files")
