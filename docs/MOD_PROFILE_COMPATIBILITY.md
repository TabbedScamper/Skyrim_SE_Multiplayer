# Multiplayer mod-profile compatibility

## Product rule

A campaign is bound to an immutable manifest revision. The server stores the
manifest and compatibility policy; it does not need to host third-party mod
archives. Each player launches an isolated local campaign profile whose final
deployed data view must match that manifest.

Human-readable mod versions are informational. Byte identity and deployment
order decide compatibility.

## Validation layers

| Layer | Compatibility evidence | Current status |
| --- | --- | --- |
| Protocol | Client/server build and manifest schema | Build checked; plugin and deployment schema v1 serialized |
| Loaded plugins | Name, ESP/ESM/ESL type, full/light slot, size, SHA-256 | Implemented in authentication |
| Manifest ownership | Frozen campaign copy that survives server restart | Implemented as a server-local pinned manifest |
| Plugin mismatch UX | Missing, unexpected, type, order, size, hash, unverifiable | Native flags implemented; lobby renders install/remove/repair groups |
| Base runtime | Skyrim executable, official master/BSA, language, SKSE, Address Library | Not implemented |
| Effective Data root | Normalized path, size, SHA-256, deterministic whole-tree root | Implemented in-process with a metadata hash cache |
| Deployed archives | Winning BSA inventory and hashes | Whole BSA hashes implemented; archive-member provenance pending |
| Loose-file view | Normalized path, winning file after mod-manager overwrite priority | Winning in-process view implemented; source-mod provenance pending |
| Native extensions | SKSE DLLs, loaders, versions, hashes, compatibility ranges | Strict DLL content matching implemented; compatibility ranges pending |
| Runtime configuration | Relevant INIs, SKSE plugin configs, MCM-derived campaign settings | Data-tree INI/TOML/YAML/CFG matching implemented; profile/game INIs and MCM pending |
| Generated output | Nemesis/Pandora/FNIS behavior and animation output, DynDOLOD-style output | Strict Data-tree behavior/assets matching implemented; generator provenance pending |
| Acquisition | Nexus/Bethesda/GitHub/manual source metadata and entitlement-aware repair | Not implemented |
| Profile isolation | Per-campaign activation without modifying the user's normal setup | Not implemented |
| Migration | New manifest revision, backup, all-player repair barrier, rollback | Not implemented |

## Plugin manifest v1

The dedicated server still owns `Data/loadorder.txt`, which defines the
required plugin names, types, and slots. It may also have the plugin files, in
which case it fingerprints them itself.

Dedicated servers often do not hold those files. If the pinned manifest is
absent, the first password-authenticated client whose names, types, and slots
match the server load order may supply the initial SHA-256 fingerprints. The
server writes them to `Campaign:sModManifestPath` without copying any mod
content. Subsequent connections and server restarts validate against that
frozen copy.

Set `ModPolicy:bAllowManifestBootstrap=false` for a public server that must be
provisioned out of band. An existing but malformed manifest is never silently
replaced from a joining client.

Changing the mod set is intentionally not an overwrite operation. It will be a
campaign migration that creates a new manifest revision and rollback point.

## Effective Data manifest v1

The client scans from inside the Skyrim process. A normal installation therefore
sees the physical `Data` directory, while an MO2 launch sees USVFS's process-local
merged view and its overwrite winners. Paths are slash-normalized and folded to
lower case, files are SHA-256 hashed, and sorted path/size/hash records produce a
deterministic whole-tree root plus roots for plugins, BSAs, scripts, native DLLs,
configuration, behaviors, and remaining assets.

The first scan hashes file content asynchronously; subsequent scans reuse a
local backing-file-identity/size/write-time cache. The cache is a performance
optimization, not an anti-cheat boundary. A campaign server pins the complete roots alongside plugin
slots and rejects incomplete scans or mismatched layers. The server never copies
or redistributes mod files.

Runtime-generated Skyrim Together directories (`backups`, `cache`,
`debug-feedback`, and `logs`) are excluded from the effective Data root. Their
contents do not affect gameplay and differ normally between PCs; including them
would make an F10 report or a local rollback backup falsely block the next join.
Legacy backup locations, `.bak`/`.pre-*` files, optional crash-report helpers,
and the host-only `STServer.ini` are excluded for the same reason. Client-side
gameplay configuration, including cell-respawn overrides, remains strict.

This proves byte-identical effective inputs, but it does not yet say which MO2
mod supplied a winning loose file or which BSA member wins at runtime. That
provenance is the next repair-UX frontier. Runtime files outside `Data` (the game
EXE, SKSE loader, Address Library, profile INIs, and Documents configuration)
also need a separate runtime manifest.
