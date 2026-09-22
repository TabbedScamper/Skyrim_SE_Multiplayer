# Deepening plan — docs/reverse-engineering/ (research-only)

Research-only. No production code, game files, saves, or configs modified.
Dirty tree preserved. Writes limited to `docs/reverse-engineering/`.

## 1. Compact evidence carried in (inspected only)

| # | Claim | Evidence (inspected body) | Confidence | Target doc |
|---|---|---|---|---|
| E1 | TiltedEvolution `dev` tip `fbf7288` 2026-09-22 #901 havok timestep | GitHub branches/dev body (prior-5, re-verified prior-7) | Verified (upstream) | SOURCE_CATALOG |
| E2 | TiltedEvolution `master` `0ffb80b` 2026-09-20 v1.8.2 #900 | GitHub commits/master body (prior-2, re-verified prior-7) | Verified (upstream) | SOURCE_CATALOG |
| E3 | TamrielOnline `master` `09ed2b2` 2017-10-21 NPC exterior+interior buffer | GitHub commits/master body (prior-2, re-verified prior-7) | Verified (upstream) | SOURCE_CATALOG |
| E4 | PR #848 open, rfortier, "Rework of party member quest progression PR #769" | GitHub pulls/848 body (prior-1, re-verified prior-7) | Verified (upstream) | SAVE_QUEST_WORLD_STATE |
| E5 | PR #769 closed/merged 2025-10-17, merge `d7797cd`, author otsffs | GitHub pulls/769 body (prior-1, re-verified prior-7) | Verified (upstream) | SAVE_QUEST_WORLD_STATE |
| E6 | Fork rfortier/TiltedEvolution-rwf `features-integration` `a7d0615` 2026-03-02 | GitHub API fork:true + branch body (prior-1) | Reported | SOURCE_CATALOG |
| E7 | License: project GPL-3.0-or-later + `code/launcher` LGPLv2 carve-out; API spdx NOASSERTION | /license endpoint base64-decoded (prior-1/5) | Verified (upstream) | SOURCE_CATALOG |
| E8 | TamrielOnline GPL-3.0 LICENSE 35141 B; `SkyBrothers.psc` 0.033 s pulse, `Commands.h` SKSE bridge, `GameEvents.h` | Raw LICENSE + source bodies (prior-2) | Verified (upstream) | SOURCE_CATALOG |
| E9 | TiltedEvolution raw LICENSE 579 B; `Code/{client,server,encoding,common}` tree; `ActorValueService.h` entt dispatcher | Raw LICENSE + contents API (prior-2) | Verified (upstream) | SOURCE_CATALOG |
| E10 | Epoch ownership: `CharacterService.cpp` Assign/Transfer/Claim/Relinquish, `OwnerComponent.OwnershipEpoch`, reasons LeaderAssignment/LeaderClaim/Mount/Relinquish/OwnerUnavailable | File bodies at `dev` (prior-5) | Reported (truncated remainder) | NETWORK_AUTHORITY_MAP |
| E11 | Quest sync merged (#769): `QuestService.cpp` IsInParty gate, `PlayerService.cpp` dialogue gate, `Actor.cpp`/`TESObjectREFR` quest-item retention, `ObjectService` assign-resp | /pulls/769/files pp1-8 (prior-1) | Reported (truncated remainder) | SAVE_QUEST_WORLD_STATE |
| E12 | Quest rework unmerged (#848 head `112493d`): `QuestStageDedupHistory` bodies | Ref `112493d` bodies (prior-1) | Reported (truncated remainder) | SAVE_QUEST_WORLD_STATE |
| E13 | Engine seams: `BGSSaveLoadManager` Save/Load IDs 34818/35727, 34819/35728, LoadMostRecent 34856/35766, singleton 516860/403340; `TESQuest::CreateRefHandleByAliasID` 24537/25066; SKSE SerializationInterface | CommonLibSSE-NG CharmedBaryon headers+cpp, Offsets.h (prior-4) | Verified 1.6.1170 / PROBE-1.7.104 | SAVE_QUEST_WORLD_STATE, RUNTIME_1_7_104 |
| E14 | Failure matrix: `PlayerCharacter::RespawnPlayer` before `Send(PlayerRespawnRequest)`; server rebroadcast-or-resync; quest relay no leader gate; alias fill local | `PlayerService.cpp:229-233`, `PlayerCharacter.cpp:89-135`, server `CharacterService.cpp:506-544`, `PlayerService.cpp:153-203`, `QuestService.cpp:55-151` + server `QuestService.cpp:23-98` (prior-6) | Reported (truncated remainder) | ACTORS_DEATH_RESPAWN, SAVE_QUEST_WORLD_STATE |
| E15 | Live-probe bridge: pipe `\\.\pipe\SkyrimSEMultiplayer.Test`, WM_APP+0x51B, 15 s timeout, snapshot fields, `Run-InGameTests.ps1` artifacts/ | `GameTestService.cpp:117-311`, `.h:11-14`, cef-cdp.mjs (prior-3) | Reported (local-repo only, unreadable this session) | LIVE_PROBES |
| E16 | Workspace root `C:/Users/mwalt/SkyrimSeamlessCoop` present; `package.json` TiltedEvolution manifest; zero hits for `primary-0` / `evidence result\|workflow` | File listing + direct read (prior-8) | Verified (local) | INDEX |

Mojibake sweep this turn: regex `Ã|Â|�|ï¿½|â€|ðŸ` over `docs/reverse-engineering/` returned zero hits. No fix needed.

## 2. Blockers (INDEX.md mirror)

1. Truncated remainders of prior-1/2/3/4/5/6 (primary-1..5 + critic): summaries cut by `[summary truncated]`; bodies in section 1 rows E10–E15 still need direct re-inspection before any "Verified" claim upgrades.
2. `primary-0`: no item definition and no evidence result in refs; cannot inspect source without the assigned question/claim + evidence scope.
3. Local-repo oracle gap: `GameTestService.cpp/.h`, `cef-cdp.mjs`, `Run-InGameTests.ps1`, BACKLOG slices beyond §9 — PowerShell sandbox unavailable this session (`SetNamedSecurityInfoW 1340`), so only `read_file`/`search` paths worked; live-probe claims stay Reported.
4. Version pinning: AE IDs above are 1.6.1170 lineage; every 1.7.104 use needs Address Library `versionlib-1-7-104-0.bin` live-probe confirmation (PROBE-1.7.104).

## 3. Next 3 slices (ordered, acceptance-evidence each)

Slice A — Pin + license pass (unblocks BACKLOG #1).
Fetch at pinned refs (`dev fbf7288`, `master 0ffb80b`, PR848 `112493d`, PR769 merge `d7797cd`): `World.h`, `GameServer.cpp`, `CharacterService.h`, `QuestService` client+server, `QuestStageDedupHistory`, raw LICENSE bodies. Write SHAs + byte sizes + header paths into SOURCE_CATALOG; set license lines to E7 (GPL-3.0-or-later + LGPLv2 carve-out, NOASSERTION). Acceptance: every row E1–E9 re-fetchable from primary URL + commit.

Slice B — Quest/authority convergence (unblocks BACKLOG #6, P5).
Re-inspect E11/E12/E14 bodies end-to-end: client send gates, server relay vs leader gate, dedup-history semantics, respawn-before-send ordering. Record exact divergence fields into SAVE_QUEST_WORLD_STATE + NETWORK_AUTHORITY_MAP gap §4 + ACTORS_DEATH_RESPAWN. Acceptance: follower-convergence criterion (P5) named per field, no "Verified" without body lines.

Slice C — Engine-seam revalidation (unblocks BACKLOG #1–3, P1–P3).
Re-inspect E13 headers/cpp + Offsets.h numerics; mark each ID 1.6.1170-confirmed vs PROBE-1.7.104; tie P1–P3 bridge reads to IDs 403560/52742. Acceptance: RUNTIME_1_7_104 table shows per-ID status + probe that would flip it.
