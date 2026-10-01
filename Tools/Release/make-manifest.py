#!/usr/bin/env python3
"""Describe the exact Data-rooted payload shipped in a release."""

import argparse
import datetime
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOTS = {
    "SkyrimTogetherReborn",
    "Skyrim_SE_Multiplayer",
    "SkyrimTogetherRebornBehaviors",
    "Skyrim_SE_MultiplayerBehaviors",
    "scripts",
    "meshes",
}
ROOT_FILES = {"SkyrimSEMultiplayer.esp", "SkyrimSEMultiplayerQuestPatches.esp"}
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?\Z")


def safe_path(value):
    parts = value.split("/")
    return (
        value
        and len(value) <= 260
        and "\\" not in value
        and ":" not in value
        and ".." not in value
        and all(parts)
        and (value in ROOT_FILES if len(parts) == 1 else parts[0] in ROOTS)
    )


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("payload", type=Path)
    parser.add_argument("version")
    parser.add_argument("notes", type=Path)
    parser.add_argument("zip", type=Path)
    parser.add_argument("zip_url")
    parser.add_argument("--output", type=Path, default=Path("latest.json"))
    args = parser.parse_args()
    if not VERSION.fullmatch(args.version):
        parser.error("invalid release version")
    if not args.zip_url.startswith("https://github.com/TabbedScamper/Skyrim_SE_Multiplayer/releases/download/"):
        parser.error("zip URL must be a versioned release asset")
    if not args.payload.is_dir() or not args.zip.is_file():
        parser.error("payload and zip must exist")
    files = []
    seen = set()
    for path in sorted(args.payload.rglob("*")):
        if path.is_symlink():
            parser.error(f"symlink in payload: {path}")
        if not path.is_file():
            continue
        relative = PurePosixPath(*path.relative_to(args.payload).parts).as_posix()
        if not safe_path(relative) or relative.lower() in seen:
            parser.error(f"invalid or duplicate Data path: {relative}")
        seen.add(relative.lower())
        files.append({"path": relative, "size": path.stat().st_size, "sha256": digest(path)})
    required = {
        "SkyrimTogetherReborn/SkyrimTogether.exe",
        "SkyrimTogetherReborn/Skyrim_SE_Multiplayer.exe",
        *ROOT_FILES,
    }
    if not required.issubset({entry["path"] for entry in files}):
        parser.error("payload is missing a launcher, starter, or ESP")
    with zipfile.ZipFile(args.zip) as archive:
        names = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
        if len(names) != len(files) or set(names) != {entry["path"] for entry in files}:
            parser.error("zip contents differ from staged payload")
        for entry in files:
            with archive.open(entry["path"]) as stream:
                sha = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    sha.update(chunk)
                if sha.hexdigest() != entry["sha256"]:
                    parser.error(f"zip hash differs: {entry['path']}")
    manifest = {
        "schema": 1,
        "version": args.version,
        "publishedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "notes": args.notes.read_text(encoding="utf-8"),
        "zip": {"url": args.zip_url, "size": args.zip.stat().st_size, "sha256": digest(args.zip)},
        "files": files,
    }
    args.output.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
