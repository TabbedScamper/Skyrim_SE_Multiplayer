# Backlog — ordered slices

Dependency order; each slice lists acceptance evidence and rollback risk.
`AGENTS.md` applies: prior-art research recorded, in-game confirmation before
any "fixed" claim.

1. **Pin versions.** Record `TiltedEvolution/dev` SHA; pin CommonLibSSE-NG
   commit; re-verify VM slots, `Main::Update`, `IsInBleedout`.
   Evidence: SHAs + header paths in SOURCE_CATALOG. Risk: none (docs only).
2. **Bridge P1–P2.** Read singleton/FOV; camera reapply after resize.
   Evidence: before/after F9 reports. Rollback: delete probe commands.
3. **Pause matrix (P4) + VM load (P3).** Evidence: documented matrix +
   thresholds. Risk: none (observational).
4. **Menu-stack snapshot.** Structured open-menu/flags/input-context feed over
   the pipe. Evidence: snapshot matches F9 menu section during transitions.
   Rollback: feature-flag the feed.
5. **Epoch ownership prototype (actors only).** Leader assign/claim/relinquish
   per `CharacterService` vocabulary; owner-unavailable reassignment.
   Evidence: P7 passes for movement + death states. Rollback: single authority
   module behind a session flag; default off = single-player behavior.
6. **Quest-stage relay.** Leader-owned stage advance; follower convergence
   (P5). Evidence: journal/alias snapshots match. Risk: quest breakage —
   gate behind allow-listed quests.
7. **Cell authority + reset clocks.** Leader-owned transitions/resets (P6).
   Evidence: reference sets match across crossing orders. Risk: world-state
   corruption — snapshot saves before each test; never on player saves.
8. **Inventory transactions (P8).** Leader-serialized loot/drop/trade.
   Evidence: bit-identical ledgers, no dupes across race tests. Risk: item
   loss/dupe — test on throwaway saves only.
9. **Leader migration design.** Epoch-bump + World transfer on leader loss.
   No prior art verified: design doc first, then prototype. Evidence: session
   survives scripted leader kill. Risk: split-brain — hard requirement of
   single-writer invariant review before merge.
