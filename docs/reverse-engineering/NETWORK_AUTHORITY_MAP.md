# Network authority map — STR paths vs leader-authoritative target

## STR architecture — Verified (upstream, `dev` branch, SHA unpinned)

- Dedicated-server model: standalone `SkyrimTogetherServer.exe` + game
  clients (corroborated by server-hosting docs and repo layout
  `Code/{server,client,common,admin*}`).
- Server `World` = `entt::registry` + dispatcher; context: Player / Party /
  Character / Calendar / Quest / ScriptService (partially verified via
  `World.h`; full composition Reported).
- Ownership is **epoch-based with explicit transfer reasons**
  (`CharacterService.h`, fetched this run):
  `LeaderAssignment | LeaderClaim | Mount | Relinquish | OwnerUnavailable`,
  gated by `CanClaimOwnership(player, entity, expectedEpoch, reason)`.
- Verified message/event surface (header-level): `AssignCharacterRequest` /
  `AssignCharacterResponse`, `RequestOwnershipTransfer`,
  `RequestOwnershipClaim`, `OwnershipTransferEvent`,
  `ClientReferencesMoveRequest`, `CharacterExteriorCellChangeEvent`,
  `CharacterInteriorCellChangeEvent`, `CharacterSpawnedEvent`,
  `CharacterRemoveEvent`, `RequestRespawn`, `DialogueRequest`,
  `SubtitleRequest`, `RequestFactionsChanges`, `SyncExperienceRequest`,
  `NewPackageRequest`, `MountRequest`.

## Gap analysis: STR dedicated server → party-leader-authoritative seamless co-op

1. **Host = player.** STR's server is headless and always available; our
   leader runs a full game simulation AND hosts. Load/VM-overstress behavior
   (MAIN_LOOP_AND_THREADING) becomes a session-health issue, not just perf.
2. **Leader migration.** STR has `OwnerUnavailable` reassignment but no
   host-migration concept (server never leaves). We need leader leave/crash →
   a follower promotes with epoch bump + full `World` transfer. No prior-art
   path verified; green-field design required.
3. **Join-in-progress state dump.** STR clients connect to a persistent world;
   our joiners need a snapshot of leader-owned quest/cell/inventory state.
   Save-format vs live-snapshot decision is open (see SAVE_QUEST_WORLD_STATE).
4. **Cell/quest authority scope.** STR relays cell-change events; whether it
   gates quest stages and reset clocks server-side is unverified — exactly the
   clocks our leader must own.
5. **Seamlessness.** STR uses lobby/server browser flows; our session must
   survive fast-travel, cell transitions, and menu pauses without re-lobbying.
   UI pause/menu state (UI_SCALEFORM_MIST) is therefore session state.

## Adoptable as-is (pending license + SHA pin)

Epoch ownership + transfer-reason vocabulary; per-domain service split;
registry+dispatcher world model; named-pipe/JSON bridge pattern (via Skyrim
MCP precedent) for the local debug bridge.
