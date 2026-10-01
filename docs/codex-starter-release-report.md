# Starter, release, and installer edit report

Edited files only. No xmake build, game run, installer run, or commit was performed.

## Changes

- Added `Code/starter/xmake.lua` and `Code/starter/main.cpp`, included from `Code/xmake.lua`. The GUI starter checks running Skyrim processes, parses bounded pending metadata, validates release versions and Data paths, checks staged SHA-256 hashes with BCrypt, journals copies and backups, restores after failed or interrupted applies, renames its running executable before replacement, writes `version.json` last, keeps one backup folder, and launches `SkyrimTogether.exe` with forwarded arguments.
- Added `Tools/Release/make-manifest.py` and `Tools/Release/check-payload.py`. They validate the staged Data tree, verify exact zip contents and hashes, and emit `latest.json`.
- Updated `.github/workflows/release.yml`, `windows.yml`, `windows-playable-build.yml`, and `linux.yml`. Tags supply `SSM_RELEASE_VERSION`; the Data-rooted zip is `Skyrim_SE_Multiplayer-<version>-windows-x64.zip`. CI stages the payload, generates notes once, builds the installer, and publishes the zip, symbols, installer, manifest, and Linux tarballs. Tag builds now use this fork's `main` branch.
- Added `Installer/Skyrim_SE_Multiplayer.iss` for Steam library detection, game-version warning, writable Data check, staged payload install, Microsoft-signed VC++ redist bundle, per-user `plugins.txt` backup and updates, and shortcuts.
- Added source and review notes to `docs/REFERENCE_RESEARCH.md`.

## Limits

- No changed build, installer, or game behavior was exercised. Runtime success and crash rollback remain unverified.
- `Code/client/Services/Generic/UpdateService.cpp` was left unchanged as instructed. Its ANSI serialization of non-ASCII staged paths can make updates fail or terminate the in-game worker.
- A per-user installer stops at protected Skyrim Data folders; use a writable Steam library.
- Reviewer A completed a read-only review; findings were addressed. Reviewer B (Muse) could not start because its auth settings file was inaccessible.
- The requested `C:\Tools\skyrim_re\agent\codex-starter-release-report.md` could not be written because workspace permissions reject writes outside this repository. This copy is inside the allowed workspace.
