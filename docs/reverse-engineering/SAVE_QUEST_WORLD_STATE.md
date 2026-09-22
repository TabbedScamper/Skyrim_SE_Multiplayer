# Save / quest / world state

## Save/load lifecycle — Unknown (top risk)

- No verified save entry point yet. Expected surface (Hypothesis):
  `BGSSaveLoadGame` + per-form `Save`/`Load`/`Revert` via `TESForm` vtables,
  `TESDataHandler` as the form registry. None demonstrated on 1.7.104.
- A party-leader-authoritative session must own: when saves happen, whose
  save file is canonical, and how joiners reconcile (full state push vs
  save-file transfer). STR's answer is server-side `World` persistence, not
  client save files — see NETWORK_AUTHORITY_MAP.

## Quests — Hypothesis with one upstream anchor

- Verified (upstream): TiltedEvolution has per-domain server services
  (`CalendarService`, `ActorValueService`, …) and a Quest-context in server
  `World`; quest state lives server-side, not on clients.
- Unverified: quest-stage message set, alias resolution across clients,
  scene/dialogue gating (`DialogueRequest`/`SubtitleRequest` exist in
  `CharacterService.h` — Verified upstream — so dialogue at least passes
  through the server).
- What the leader must own: stage advancement, alias fills, active quest
  objectives, per-player quest divergence policy (shared journal vs instanced
  stages — design decision, prompt P5 covers only observation).

## Globals, cells, persistence — Hypothesis

- Expected: `TESDataHandler` globals, cell attach/detach on grid move,
  `CharacterExteriorCellChangeEvent` / `CharacterInteriorCellChangeEvent`
  (Verified upstream in `CharacterService.h`) as the cell-authority seams.
- Reset/respawn timers and encounter zones: Unknown. STR-side equivalent
  unverified. Risk: clients whose cells reset on different clocks will
  duplicate or delete world objects (see INVENTORY drop hazards).
- Persistence boundary rule (working hypothesis): leader owns respawn/reset
  clocks and persisted references; clients treat cell contents as read-mostly
  replicas except for owned actors.

## Acceptance bar

No implementation slice may claim quest/world authority until a live probe
shows a follower's quest stage and cell contents converging on the leader's
after a stage advance + cell transition (probes P5/P6).
