# Actors / death / respawn

## Engine surfaces — mostly unresolved

- `Actor::IsInBleedout` ID 48461 (SE only) — Reported, single-source; needs
  SE/AE/VR triple check before any bleedout-sync hook.
- `Actor::Kill` / resurrect vtable slots, life-state/ragdoll transitions —
  Unknown. Do not cite offsets seen in secondary sources.
- Encounter-zone and cell-reset interplay with death cleanup — Unknown;
  same clock-ownership problem as SAVE_QUEST_WORLD_STATE.

## STR-side anchors — Verified (upstream)

- `CharacterService.h` (`dev`, fetched this run) handles:
  `RequestRespawn`, `OnRequestRespawn`, `CharacterRemoveEvent`,
  `CharacterSpawnedEvent`, `CharacterSpawnRequest`/`Serialize`,
  `ClientReferencesMoveRequest`, `RequestOwnershipClaim` /
  `NotifyOwnershipTransfer`-family messages, and `MountRequest`.
- Ownership transfer reasons include `OwnerUnavailable` — i.e. STR already
  reassigns dead/disconnected owners' actors. Our leader-authoritative port
  can treat leader reassignment as the same primitive.

## Candidate sync hooks (hypotheses, probe-gated)

1. Death event → ownership freeze on the corpse actor; killer/credit
   attribution replicated as data, not derived per-client.
2. Bleedout (essential NPCs/players) as a replicated state with a leader
   countdown, not per-client timers.
3. Respawn as leader-issued spawn assignment (`AssignCharacterRequest` analog),
   never client-decided.
4. Player respawn = leader-owned load/position event; follower clients must
   not trigger independent cell resets on respawn.

## Acceptance bar

Probe P7: kill a shared actor on one client; leader and follower must agree
on dead/alive, lootable corpse identity, and respawn timing within one reset
cycle. Disagreement = authority gap, not a tuning issue.
