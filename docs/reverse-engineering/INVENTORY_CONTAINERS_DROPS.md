# Inventory / containers / drops

## Identity model — Hypothesis (highest dup-risk area)

- Expected engine surface (all unverified on 1.7.104): `TESObjectREFR`
  inventory + `ExtraDataList` (enchantments, health, ownership, quest-item
  flags), container `AddItem`/`RemoveItem` paths, `PickUp`/`Drop` object
  creation for dropped references.
- No inventory/container Address Library IDs verified this run.

## STR-side anchors — partial

- `CharacterService::BuildActorData` / `Serialize` and
  `ClientReferencesMoveRequest` (Verified upstream, header-level) imply actor
  equipment/appearance replication; full item-transfer message set unverified.
- `RequestFactionsChanges` exists server-side (Verified upstream) — faction/
  ownership-adjacent state passes through the server.

## Duplication hazards (design constraints, not fixes)

1. Two clients looting the same container concurrently → both `RemoveItem`
   locally → item exists twice. Container take must be a leader-serialized
   transaction.
2. Drop-then-pickup races create new references per client; dropped-reference
   identity must be leader-issued.
3. ExtraData divergence (charge/health/ownership flags) silently forks item
   identity; replication must include the ExtraData delta or forbid transfer
   of flagged items in early slices.
4. Vendor/merchant gold + respawn chests interact with reset clocks (see
   SAVE_QUEST_WORLD_STATE) — merchant state is leader-owned.

## Acceptance bar

Probe P8: concurrent same-container loot + drop/pickup race; post-test
inventories must bit-match the leader's ledger (item, count, ExtraData flags).
Any dupe = transaction boundary missing.
