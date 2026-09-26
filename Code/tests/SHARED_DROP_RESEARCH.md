# Shared drops: implementation handoff, 2026-09-26

This is a source implementation, not a confirmed in-game fix. No build, C++ test
execution, deployment, or commit was performed. COMMON.md explicitly waives
Reviewer A/Reviewer B review for this task; neither assistant reviewed this implementation.
Research is kept here because `Code/tests` is in the task's edit allowlist and
`docs/REFERENCE_RESEARCH.md` is not. The coordinator can copy this research into
that shared document.

## Source evidence and decisions

- [TiltedEvolution InventoryService](https://github.com/tiltedphoques/TiltedEvolution/blob/dev/Code/client/Services/Generic/InventoryService.cpp),
  inspected through the raw source and locally: `OnNotifyInventoryChanges` calls
  `Actor::DropOrPickUpObject` on each remote actor. Rejected: it creates independent
  world objects without an exclusive pickup identity. Both client and server
  legacy drop-spawn branches are now disabled.
- [CommonLib TESObjectREFR](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/T/TESObjectREFR.cpp),
  `PlaceObjectAtMe`, and local `src/RE/T/TESDataHandler.cpp`,
  `CreateReferenceAtLocation`: adopted direct reference creation with the base,
  transform, cell and worldspace. Copies never pass through an actor inventory.
- [CommonLib container event declaration](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/include/RE/T/TESContainerChangedEvent.h):
  used to locate the native event, but rejected its description of field +0x10
  as a handle for this executable. The actual producer writes a reference form
  ID. The service observes that event instead of detouring DropObject again or
  guessing the nearest matching loose item.
- Local CommonLib `src/RE/E/ExtraTextDisplayData.cpp` and its header: adopted the
  native text extra layout, custom-name marker -2 and engine vtable. The stock
  `Inventory::Entry` omits custom names, so the new protocol carries a bounded
  name separately. Charge and health have explicit presence bits, preserving
  zero charge. Enchantments, custom enchantment effects, poison, soul, health and
  count are serialized with checked reads and allocation limits.
- Local `ObjectService::CaptureHostPhysics`, `OnPhysicsReferencesMove`,
  `ApplyRemotePhysics`: reuse the existing dynamic Havok follower, including its
  sample interpolation and native step targets. Shared drops have a reserved
  identity namespace admitted only from validated SharedDrop messages. The
  original leader physics relay cannot inject updates into that namespace.
- Local CharacterService cell-lease policy and `CellIdComponent::IsInRange`:
  keep the dropper's ownership while nearby; when it leaves, prefer an existing
  ready copy on the nearby leader, otherwise another nearby member. Each handoff
  increments a generation. This is not restricted to a two-player party.

## Executable inspected with sk.py (SkyrimSE.exe 1.7.104)

| Address Library ID | VA | Evidence and use |
| --- | --- | --- |
| 40454 | 0x140746E00 | Actor::DropObject calls RemoveItem with reason 3, obtains a reference handle, starts the body and returns the handle. Existing HookDropObject queues its inventory event before this result exists. |
| 37797 | 0x1406A5370 | Actor RemoveItem virtual delegates to reference inventory removal. |
| 19689 | 0x1402E7CC0 | Reference RemoveItem delegates to InventoryChanges::RemoveItem. |
| 16059 | 0x14022DA70 | InventoryChanges::RemoveItem creates dropped references on reason 3 and emits per-reference removal events. |
| 16065 | 0x1402310F0 | Drop removal event: old container, zero new container, base ID, count and actual reference form ID at +0x10; event source is holder +0x318. |
| 16149 | 0x14023F640 | General container event producer also resolves its reference before writing the form ID at +0x10. |
| 13723 | 0x1401BC1C0 | CreateReferenceAtLocation: output handle, base, position, rotation, cell, worldspace, optional existing reference, persistence flags. Used to spawn copies. |
| 19796 | 0x1402F11B0 | ActivateRef calls the base form's activation virtual at +0x1B8. The existing HookActivate receives one extra service check. |
| 17670 | 0x140278E50 | TESObjectMISC activation casts activator to Actor and calls its pickup virtual at +0x660. |
| 37521 | 0x14068B0E0 | PickUpObject transfers the actual reference's extras into inventory and retires its world reference through native task/garbage collection paths. |
| 56116 | 0x140A42DA0 | DeleteFunctor resolves the reference and delegates to GarbageCollector::Add. Copies are disabled before the existing Delete wrapper is used. |
| 11617 | 0x140160760 | ExtraDataList::SetCount adds, changes or removes ExtraCount; copied stacks use this native. |
| 11619 | 0x140160960 | SetCharge always creates/updates ExtraCharge, including zero. |
| 11616 | 0x140160630 | SetHealth creates/updates ExtraHealth; -1 is the removal sentinel and is not sent as a temper value. |
| 12315 | 0x14017E0E0 | Native ExtraDataList::Add locks and adds the node, including presence bookkeeping. Used for custom text, avoiding the local helper's null bitfield assumption. |
| 12768 | 0x140189740 | ExtraTextDisplayData::GetDisplayName recognizes custom marker -2 and temper/name state. |
| 35576 | 0x140616CE0 | Native add-change helper rejects form flag 0x4000. Proxies are excluded from change registration. |
| 14609 / 35581 | 0x1401E56B0 / 0x140616E80 | UnsetChanged delegates to native change-record removal. Called before setting the proxy temporary flag. |
| 14642 | 0x1401E6260 | SetTemporary also removes the form from the global lookup map. Rejected for linked copies because their local form IDs must remain resolvable. |
| 35583 | 0x140616EE0 | Native change-map removal helper, inspected while checking SetTemporary. |

The text-extra vtable is Address Library 186855, VA 0x1417EA638. No engine offset
was inferred from a new live probe; no new live probes were added.

## Changed files and functions

- `Code/client/Services/Generic/SharedDropService.h/.cpp`: new service. Native
  `OnEvent` observes player drop references and excludes quest aliases and
  container transfers. `TryHold` arbitrates shared activation through the server,
  with a 300 ms pending request window and no vanilla fallback. `Spawn` creates
  one native reference with extra data. `Handle` applies identities, grants,
  ownership generations and tombstones. `OnMainFrame` performs native changes on
  the existing main-frame boundary; the network worker only exchanges queues.
  `SaveOwner` excludes proxies from native change registration and enables the
  authority copy. Physics helpers expose identities and samples to ObjectService.
- `Code/server/Services/SharedDropService.h/.cpp`: new ECS drop records and
  `Gameplay:bEnableItemDrops` default true. `OnRequest` checks party, epoch,
  character ownership, positive non-quest inventory quantity, cell and distance.
  Creation debits quantity once and retains a create-token receipt. Item instance
  attributes come from the owning player's actual drop; charge/temper updates
  need not have generated an inventory event. Pickup commits a terminal claim
  before crediting inventory and notifying clients. Movement requires the current
  owner and generation. `OnUpdate` distributes late-entry snapshots, hands off
  ownership to a ready nearby copy, and retires unloaded drops to one normal
  reference on the last owner's save.
- `Code/encoding/Messages/SharedDropData.h/.cpp`, `RequestSharedDrop.h`,
  `NotifySharedDrop.h`: checked protocol, bounded extras, reserved physics
  identities, ownership generations, terminal claim policy and direction checks.
  Movement omits item data on the wire.
- `Code/encoding/Opcodes.h`, both message factories: append/register the new
  messages, preserving other message registrations.
- `Code/client/World.cpp/.h`, `Code/server/World.cpp`: service registration and
  client accessor.
- Client InventoryService `OnInventoryChangeEvent`: the real native drop event
  replaces the pre-drop inventory delta for player drops. `OnNotifyInventoryChanges`:
  the old Drop flag can no longer spawn an unlinked copy.
- Server InventoryService `OnInventoryChanges`: always sends Drop=false; setting
  registration moved into SharedDropService.
- `Code/client/Games/Skyrim/TESObjectREFR.cpp`, `HookActivate`: one early service
  check in the existing detour, before door/busy-lock handling.
- Client ObjectService capture/receive/playback/main-frame paths: include shared
  drop bodies, allow follower-owned sources and leader playback of others' drops,
  fence generations, and reject shared identities arriving on the legacy relay.
  Owned drop discovery uses explicit IDs immediately, without a follower cell scan.
- `Code/tests/shared_drops_encoding.cpp`: item-instance round trips, truncation
  rejection, packet direction and bounds, range/death validation, stale owner
  rejection, and all 120 permutations of five simultaneous pickup requesters.

## Validation status and next paired run

Passed: source registration/invariant checks and `git diff --check`. The C++
tests were added but not executed because building is expressly prohibited.
No in-game observation or five-player performance result is claimed.

Coordinator: build and run `[encoding.shared_drops]`. Then test each PC as dropper:

1. Drop one item, identical items rapidly, and a stack. Check exactly one server
   creation log per actual dropped reference: `Shared drop: {:X} x{} dropped by
   {} (id {})`. Verify count on both inventories and the same object trajectory.
2. Test a renamed, tempered, poisoned, player-enchanted weapon, including zero
   remaining charge. Pick it up with either PC, save/load, equip it, and drop it
   again. Confirm every attribute and total party quantity.
3. Activate simultaneously, including latency above 300 ms. Expect one server
   `picked up by`, no native loser pickup, a quiet `<player> took it` notice, and
   `copy removed` on losing PCs. A late final grant is delivered asynchronously;
   timeout never invokes vanilla pickup.
4. Leave the loaded cell with another player still nearby. Expect `ownership
   moved to {}`; old-owner samples must stop affecting playback. Enter late and
   confirm one copy at the latest pose. Repeat with five players and separated
   cells before profiling costs.
5. All players leave the cell: verify the last owner has one ordinary saved
   reference and no follower copy survives a save/load. Test disabling the setting,
   quest items, and putting the same item into a container (no shared drop).

## Material limits / risks to resolve in validation

- This patch has not been compiled. Native reference creation, text extra
  ownership, save exclusion and deferred deletion need the changed build exercised.
- Server drop records, create receipts and pickup claims are session memory, not
  a durable transaction journal. Disconnect or process failure while a creation,
  grant, or handoff is in flight has no durable recovery guarantee. An unreceived
  final grant can be lost on disconnect. Replica-count cleanup does not constitute
  a transactional network-partition handoff. Do not claim crash-safe persistence.
- The native observer follows the existing Actor::DropObject path (which holds
  ScopedInventoryOverride). Direct script/mod RemoveItem-with-drop calls that
  bypass that path retain their existing inventory delta behavior and are not
  made shared here. Direct mod calls to native PickUpObject that bypass Activate
  likewise do not pass the activation gate. Extending those paths requires the
  existing Actor/RemoveItem hooks, outside this task's hook edit allowance.
- Claims are terminal even if native item delivery temporarily cannot create a
  reference; the client retries while connected. Inventory rollback across loading
  an old save still relies on the surrounding campaign/inventory reconciliation.
- Snapshot traffic and receipt retention are bounded by 16,384 accepted creates
  in server memory, but have not been profiled. No five-player target is claimed.
