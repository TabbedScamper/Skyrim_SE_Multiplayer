# Reverse-engineering index — Skyrim SE seamless co-op

Research-only. No production code, game files, saves, or configs modified.
Every engine claim below carries a confidence tag:

- **Verified (local):** observed in this repo's docs, F9 reports, or local snapshots.
- **Verified (upstream):** fetched from a primary source this run (URL + commit/branch).
- **Reported:** from a research child or secondary source; not yet re-verified.
- **Hypothesis / Unknown:** explicitly marked as such.

## Map

- [SOURCE_CATALOG](SOURCE_CATALOG.md) — prior art, commits, licenses, reuse verdicts.
- [RUNTIME_1_7_104](RUNTIME_1_7_104.md) — exe hash, Address Library, symbols, confidence.
- [MAIN_LOOP_AND_THREADING](MAIN_LOOP_AND_THREADING.md) — main loop, VM update, task queues.
- [UI_SCALEFORM_MIST](UI_SCALEFORM_MIST.md) — menu stack, Scaleform/native split, Mist Menu.
- [SAVE_QUEST_WORLD_STATE](SAVE_QUEST_WORLD_STATE.md) — save/load, quests, persistence.
- [ACTORS_DEATH_RESPAWN](ACTORS_DEATH_RESPAWN.md) — death, bleedout, resurrection, resets.
- [INVENTORY_CONTAINERS_DROPS](INVENTORY_CONTAINERS_DROPS.md) — items, ownership, dup hazards.
- [NETWORK_AUTHORITY_MAP](NETWORK_AUTHORITY_MAP.md) — STR ownership/event paths, gap analysis.
- [LIVE_PROBES](LIVE_PROBES.md) — non-destructive in-game confirmation experiments.
- [BACKLOG](BACKLOG.md) — ordered slices with acceptance evidence.
- [ACCESS_NEEDED](ACCESS_NEEDED.md) — files blocked by workspace policy.
- [DEEPENING_PLAN](DEEPENING_PLAN.md) — compact evidence + blockers + next 3 slices.

## Current status (phase one)

Done this run:

- Local baseline consolidated (`UI_ARCHITECTURE.md`, `REFERENCE_RESEARCH.md`).
- Upstream correction: `tiltedphoques/TiltedOnline` is gone (404); the live
  canonical repo is `tiltedphoques/TiltedEvolution`, branch `dev`.
- Verified (upstream): server authority model —
  `Code/server/Services/CharacterService.h` (epoch-based ownership,
  `OwnershipTransferReason::{LeaderAssignment, LeaderClaim, Mount, Relinquish,
  OwnerUnavailable}`), `Code/server/GameServer.cpp` present (37,913 bytes),
  per-domain services (`ActorValueService`, `CalendarService`, …).
- Verified (local): 1.7.104 exe hash, Address Library match, Mist Menu /
  UI3DSceneManager IDs 403560 / 52742, five-layer main-menu model.

## Next tasks

1. Pin `TiltedEvolution/dev` commit SHA and fetch `World.h`, `GameServer.cpp`
   authority-relay paths, and quest/calendar/script service headers.
2. Pin CommonLibSSE-NG commit and verify VM `Update`/`UpdateTasklets` vtable
   slots, `Main::Update` RelocationIDs, `Actor::Kill`/`Resurrect` slots.
3. Verify save/load entry points (`BGSSaveLoadGame`) and quest-alias globals.
4. Run [LIVE_PROBES](LIVE_PROBES.md) P1–P3 through the debug bridge.

## Blockers (see DEEPENING_PLAN §2)

1. Truncated remainders of prior-1/2/3/4/5/6 still need direct body
   re-inspection before any claim upgrades to Verified.
2. `primary-0`: missing item definition and evidence result — needs the
   assigned question/claim + evidence scope.
3. Local-repo probe bodies (`GameTestService`, `cef-cdp.mjs`,
   `Run-InGameTests.ps1`) unreadable this session; PowerShell sandbox
   unavailable (`SetNamedSecurityInfoW 1340`).
4. AE IDs are 1.6.1170 lineage; each 1.7.104 use needs live-probe
   confirmation (PROBE-1.7.104).

## Next 3 slices (see DEEPENING_PLAN §3)

A. Pin + license pass — SHAs/sizes/headers into SOURCE_CATALOG.
B. Quest/authority convergence — divergence fields into
   SAVE_QUEST_WORLD_STATE + NETWORK_AUTHORITY_MAP + ACTORS_DEATH_RESPAWN.
C. Engine-seam revalidation — per-ID 1.7.104 status into RUNTIME_1_7_104.

## Highest-risk unknowns

1. **Save/quest ownership** — no verified save entry point or quest-stage
   authority path yet; a leader-authoritative design cannot proceed past
   actors without this.
2. **Version pinning** — most address/offset claims are not yet demonstrated
   against 1.7.104; raw offsets are candidates only.
3. **Cell reset/encounter timers** — server has no equivalent; leader must own
   reset clocks or clients will diverge.
4. **Inventory identity** — ExtraData/duplication hazards unmapped; duping is
   the most likely first multiplayer bug.
