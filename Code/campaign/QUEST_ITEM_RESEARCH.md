# Party quest items: round 6 research and implementation

2026-09-25. Candidate implementation only. No build, commit, deployment, or paired game test was performed.

The task's explicit file whitelist excludes `docs/REFERENCE_RESEARCH.md`. This report is kept under the permitted `Code/campaign` directory for the coordinator to incorporate into that document. `COMMON.md` explicitly exempts this worker from the Reviewer A/Reviewer B review rule; neither assistant reviewed this change.

## Source inspection and decisions

Web search preceded implementation. Primary CommonLib source was opened online and compared with the local `C:/Tools/ref/CommonLibSSE-NG` checkout (`a898f469851c464d05137bb74b069dd234897643`). Bethesda-derived scripts and executable output remain outside the repository.

| Reference inspected | Finding and decision |
| --- | --- |
| [InventoryEntryData.cpp](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/src/RE/I/InventoryEntryData.cpp), `IsQuestObject` | Walks instance extra lists and calls `HasQuestObjectAlias`. Adopt the native representation; a bool in a network inventory entry is insufficient. |
| [ExtraDataList.cpp](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/src/RE/E/ExtraDataList.cpp), `HasQuestObjectAlias` | Native relocation, with reference-handle indirection confirmed in the exe. Follow the instance reference instead of reading only the base form. |
| [ExtraAliasInstanceArray.h](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/include/RE/E/ExtraAliasInstanceArray.h) and its .cpp | Entries hold quest, alias, and optional instanced-package pointers; the array owns its entries. Use the engine's allocator/append path. Reject memcpy/sharing another extra list's owned storage. |
| [BGSBaseAlias.h](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/include/RE/B/BGSBaseAlias.h) and its .cpp | Quest-object is alias flag 0x4. An unflagged item alias is still an acquisition under the agreed definition. Do not change authored alias flags to make all items quest objects. |
| Local `Code/client/Games/Skyrim/TESObjectREFR.cpp:592`, `GetItemFromExtraData` | Existing packets capture `IsQuestItem` through the native check. |
| Same file, `GetExtraDataFromItem` and `AddOrRemoveItem` | Received extras do not recreate quest aliases. The old recursive remote-player pickup helper creates a base copy without reproducing quest protection. The new service bypasses only that opportunistic recursion when handling party quest inventory notifications. |
| Local `Code/server/Services/InventoryService.cpp`, `OnInventoryChanges` | Non-owners cannot mutate another player inventory; NPC looting is a separate permitted case. Leave this authority rule intact. Durable entitlements are separate from actor inventory deltas. |
| Local `Code/server/Services/QuestService.cpp`, `ProcessQuestChanges` | Existing quest journal is campaign-scoped and leader-authoritative. Use the same SQLite ledger, but make the quest-item projection and its journal entry atomic. |

## Executable evidence: SkyrimSE.exe 1.7.104

Queried with the specified Python interpreter and `C:/Tools/skyrim_re/sk.py`. `sk.cmd` did not print useful output in this shell; direct Python queries worked. These were static reads, not live probes or guessed hook offsets.

| Address Library ID | VA | Relevant native behavior |
| --- | --- | --- |
| 16005 | `0x140228E90` | `InventoryEntryData::IsQuestObject` / project `ExtraContainerChanges::Entry::IsQuestObject`: loops extra lists and calls 12052. |
| 12052 | `0x140172D30` | `ExtraDataList::HasQuestObjectAlias`: reads extra 0x1C, resolves handle, follows reference extra list at +0x70 when reference flags have bit 0x20, then tests extra 0x88. This bit is reproduced as observed, not relabeled as a persistent-form flag. |
| 12731 | `0x140188780` | Alias array quest-object test: holds read lock at +0x28; each instance's alias pointer is +8; tests alias flags +0x1C bit 2. |
| 12720 | `0x140187920` | Alias visitor: read-locks the array, invokes visitor slot 1 with alias; return 1 continues. Collect pointers while locked, inspect quest state after the visit. |
| 19656 | `0x1402E6A70` | Reference `ForEachAlias` uses the same reference-handle indirection and the 12720 visitor. Existing TriggerGate uses this ABI. |
| 12046 | `0x1401728E0` | Add alias to extra list: locks, creates native ExtraAliasInstanceArray if needed, delegates to 12718. Used for copies without rebinding the actual quest alias. |
| 12718 | `0x140187650` | Allocates an owned 0x18-byte alias instance and inserts into the native array under write lock. Arguments are quest, alias, and instanced-packages pointer. |
| 24011 | `0x14039FDE0` | Alias attach path calls 19652 (`0x1402E6810`), which calls 12046. Also reads quest +0x50 as the current instance, using next instance during pending startup. Discovery waits out startup. |
| 25051 | `0x1403D4BC0` | Clear reference alias: cleans the quest's handle map, alias attachment, reservation and persistence. Observe this instead of changing authored alias flags. |
| 25052 | `0x1403D4FA0` | ForceRefIntoAlias: returns if the handle is unchanged, otherwise calls 25051 then assigns the new handle. The deferred release callback checks the final binding, avoiding transient same-reference clears. |
| 24991 | `0x1403D0630` | SetCompleted: toggles quest +0xDC bit 1, marks changed, and emits QuestStatus. Completion retirement is tied to the observed quest instance. |
| 56218 | `0x140A48340` | Papyrus RemoveItem native only constructs a delayed functor. Do not treat a queued call as a successful removal. |
| 56098 | `0x140A418E0` | RemoveItemFunctor slot 1 executes the removal. Owner handle is +0x10; requested form +0x18. Base items/form lists and reference items take different branches. The hook compares tracked base counts before/after the execution and checks surviving alias instances. |
| 56145 | `0x140A44BB0` | Papyrus AddItem accepts a TESForm pointer, count and silent bool and queues the native AddItemFunctor. Passing the real reference preserves its identity and scripts. Use the registered native rather than an invented extra-reference handle. |
| 51636 | `0x140935D60` | Native added/removed HUD notice: base form, count, added bool, sound bool, optional name. Uses the game's localized Added/Removed strings. |

## Concrete script and record audit

Extracted files were available in `C:/Tools/ref/scripts-all/psc/Skyrim - Misc/scripts`. The parsed master record cache `C:/Tools/ref/scripts-all/records.pkl` was also inspected read-only.

### Golden Claw

The Lucan claw quest is **MS13 (`00039645`)**, not MQ103. MQ103 is related main-quest content at Bleak Falls Barrow; the claw's actual pickup and return logic is in MS13.

* MS13 alias 11 `GoldenClaw` has FNAM `0x4`, ALCO base `00039647`, created in Arvel's alias inventory. Alias 17 `LucanClaw` uses the separate display reference `000AC9B6`, without quest-object flag.
* `ms13goldenclawscript.psc:15-18`: alias `OnContainerChanged` compares the new container with `Game.GetPlayer()` and sets MS13 stage 40. A base copy alone cannot reproduce the real reference's alias event; move the real reference into the leader's inventory.
* `qf_ms13_00039645.psc:60`: the Lucan completion fragment removes one **base-form** GoldenClaw from `Game.GetPlayer()`, enables the display claw, and gives the reward. The alternate Camilla fragment at line 187 also removes by base form.
* Follower copies make base-form `GetItemCount`/`OnItemAdded` logic possible. They deliberately do not steal the alias binding from the leader's reference. A script that insists the unique alias reference's container equal every local player cannot be satisfied simultaneously by this agreed ownership model. Such reference/alias side effects belong to the leader; followers depend on quest-state replication.

### Helgen keep key

* MQ101 (`0003372B`) binds `MQ101KeepDoorKey` to base `000B5178` in its player alias script properties (VMAD object property, alias -1).
* `mq101playerscript.psc:16` installs an inventory event filter for that **base form**.
* Lines 25-27 advance stage 260 on `OnItemAdded` when the base form matches. This is not an item alias script.
* MQ101's 135 reference aliases contain no key alias; the parsed Skyrim quest records have no ALCO creating `000B5178` as an alias item. The player's script property/filter is not ExtraAliasInstanceArray on the key.
* Therefore the agreed alias/quest-object definition does **not** generally select this ordinary key. No MQ101 or form-specific exception was added. Sharing all script-relevant base forms or all keys would require an explicitly broader generic acquisition rule.

No `GetContainer` call was found in the extracted PSC corpus searched. Do not confuse CommonLib/TESObjectREFR's C++ `GetContainer()` (base container definition) with an inventory item's containing reference. This implementation checks actual inventory reference handles. The concrete claw's source proves the container-sensitive requirement through `OnContainerChanged`.

## Changes

* `Code/client/Services/Generic/QuestItemService.h/.cpp`: new service; `Discover` reads native item alias membership, `Send` retries stable transaction tokens, `OnNotify` merges sequenced durable revisions and complete snapshot pages, `OnUpdate` gates loading/session changes, and `Reconcile` restores missing entitlements with native alias extras and notices. Multiple aliases on the same reference are attached to one copy. One alias reference represents one instance, not the whole unrelated base stack.
* Same client service: `HookRemove` observes actual delayed Papyrus removal including form lists; `HookClear` defers alias-release evaluation; `HookComplete` records completion; `ScriptRemoved`, `AliasReleased`, and `QuestCompleted` issue leader-only retirement. Retiring items are not restored while awaiting their durable acknowledgment. Death, ordinary missing-item counts and disconnects do not retire entitlements.
* Same client service: leader recovery passes the real alias reference to native AddItem. A follower carrying the real reference moves it to the leader's local proxy when available, then receives its base copy. If a leader reference is unresolved, a protected interim copy preserves base-item access while the service waits to replace it with the real reference.
* `Code/client/Services/Generic/InventoryService.cpp`: include and one call to `HandleInventoryNotify`. The new method preserves remote epoch checks and ordinary inventory application but skips the old unflagged local-copy recursion. No other inventory-service logic changed.
* `Code/client/World.cpp/.h`: registration/include and declaration. `Code/server/World.cpp`: server registration/include.
* `Code/server/Services/QuestItemService.h/.cpp`: loads durable records; validates membership, started session and epoch; accepts party acquisition as set membership; accepts consumption only from leader; commits before broadcasting to every member including picker, across cells. `SendSnapshot` sends 64-record pages. One configured ledger is one campaign; a second concurrent party cannot access the first party's quest-item service.
* `Code/encoding/Messages/QuestItemState.h/.cpp`, `RequestQuestItems.h/.cpp`, `NotifyQuestItems.h/.cpp`: fixed-width bounded encoding, malformed/truncated packet rejection, active/tombstone state, per-record revisions, request tokens, session epochs, snapshot page sequencing. Identity includes quest instance to separate repeated quests from old-save replay.
* `Code/encoding/Opcodes.h`, `ClientMessageFactory.h`, `ServerMessageFactory.h`: register the two messages while retaining other engineers' entries.
* `Code/campaign/CampaignLedger.h/.cpp`: additive `quest_items` table keyed by pinned base/quest plugin IDs, alias and quest instance; persistent reference identity when available; quantity, native flag, active/tombstone and journal revision. `SetQuestItem` does idempotency and revision compare-and-set with projection and journal insertion in one transaction. `CommitLocked` is the existing commit body factored for transaction reuse. SQLite foreign key prevents an item revision without a journal row.
* `Code/campaign/tests/CampaignLedgerTests.cpp`: five new cases cover restart/hand-in persistence, stale and contradictory writes, five concurrent acquisitions of one entitlement, campaign and alias isolation, and repeated quest instances.
* `Code/tests/quest_items_encoding.cpp`: five protocol cases cover round trips, all byte truncations, bounded pages, duplicate identities, malformed authority/payloads, and repeated instances. These are serialization/ledger tests, not a claim of server integration or five-player performance validation.
* `Code/campaign/tests/quest_item_schema_check.py`: runs SQLite constraints and rollback using the actual schema strings extracted from CampaignLedger.cpp without compiling C++.

## Validation and next paired run

Executed: six SQLite schema checks, all passing. `git diff --check` passed for the touched tracked paths. Inspected message factory registration, source-level native ABI/layout evidence and build glob inclusion. The C++ tests and client/server have **not** been built or run. No in-game result is claimed.

Coordinator validation after building:

1. Have the follower pick up the Golden Claw. Server: one `Quest items: acquired ... alias=11` revision. Host: `queued real reference` followed by actual possession of that reference; MS13 reaches 40 through its container event. Follower: count one, Quest Items tab/native quest protection, normal localized added notice. Repeat with host picking it up and members in another cell.
2. Hand in to Lucan. Server: `handed in` with a later revision. Follower: `removed party copy`. Neither side restores it during later scans. Reconnect/Continue an older save containing the claw: the tombstone must remove the stale tagged copy.
3. Remove a copy via death fallback or a permitted test-only inventory removal outside Papyrus RemoveItem. It should be restored, with no server hand-in revision. Exercise another-player pickpocketing, leader/follower disconnect, join and loading-menu boundaries.
4. Clear and rebind an alias, stop/complete a quest, restart a repeatable quest, and test one object reserved by two aliases plus two distinct objects sharing a base. Confirm no duplicate notices/count growth and that an active reservation survives retirement of another.
5. Watch `recovery snapshot ready` after every join/load/session epoch change. A repeated `queued real reference` without eventual possession, `interim copy ... unresolved`, or `durable mutation/snapshot failed` is unresolved behavior, not success.
6. After paired correctness passes, measure CPU scan cost, allocations and bytes for **five simultaneous players**, then larger parties. The 500 ms scan and 64-row snapshot pages are initial choices, not profiled performance claims.

## Remaining risks and limits

* Native alias extras on base copies, their save/load reconstruction, native reference transfer, and hook ABI must be exercised on the changed 1.7.104 build. Static source agreement is not runtime validation.
* Base-form count and player item events work independently of unique-reference container tests. Follower alias-reference predicates remain governed by the leader-reference policy and existing alias/quest replication.
* The service observes all successful **Papyrus** RemoveItem operations involving tracked items; it does not prove that the VM caller was specifically a quest fragment. A non-quest mod script deliberately removing a tracked item on the leader is also treated as consumption. Ordinary inventory deltas and death cleanup outside that functor are not consumption.
* A deleted/unresolvable dynamic unique reference cannot be reconstructed with its original script identity by a base copy. The explicit interim-copy warning marks that limit; the service does not invent a ForceRefTo replacement.
* Repeatable quest instances rely on the peers' quest instance counters following shared quest progression. Mismatched local quest instances and save-bound alias state require paired coverage.
* Campaign identity is the existing one-database/one-pinned-manifest model. Selecting a different campaign must select its ledger; this service does not redesign campaign selection or use transient party IDs as durable campaign keys.
* Only the agreed alias/quest-object category is covered. A script property, inventory event filter, or plot-relevant ordinary key is not automatically an engine quest item.
