Read-only audit. Do not edit any source file, build, run the game, or commit. Write your report to docs\atlas-audit-report.md in this repo (that one file only).

Context: C:\Users\mwalt\SkyrimSeamlessCoop-ui is Skyrim SE Multiplayer, a co-op mod (fork of Skyrim Together Reborn) for SkyrimSE.exe 1.7.104. The client is Code\client (C++ hooks into the game), the server is Code\server, the wire format is Code\encoding. C:\Users\mwalt\SkyrimAtlas is a reverse-engineering atlas of the same exe, built overnight:
- for-coop\ANSWERS.md: findings written for this mod (read all of it first).
- systems\*.md: 22 engine system documents.
- hooks\catalog.json and hooks\*.md: 236 entries, each with trigger function (VA + Address Library ID), thread class, state fields, how to reproduce on another PC, what must not fire twice, persistence (change flags), hazards.
- graph\field_index\: every read/write of 85 key structs with writer threads.
- tables\: change flags, locks, task codes, actor values, form types, settings and more.
The decompiled exe is queryable: C:\Tools\skyrim_re\sk.cmd id|fn|callers|callees|find|grep. The mod finds functions by Address Library ID with POINTER_SKYRIMSE(type, name, ID) and hooks them with TP_HOOK.

Task: find where the Atlas shows the mod is wrong, fragile, or missing something, and where it enables a concrete improvement. Cross-check against the actual code. Cover at least:
1. Every POINTER_SKYRIMSE / raw offset in Code\client whose ID, signature, offset or meaning the Atlas contradicts (offsets that changed in 1.7.104 vs CommonLib, wrong struct sizes, wrong field names, wrong thread assumptions).
2. Every hook in Code\client whose target the Atlas says runs on a worker thread, under a lock, or reentrantly, where the hook does unsafe work (Papyrus calls, UI calls, logging under locks, caching AIProcess/MiddleHigh/High pointers across engine calls, MoveTo or SetProcessLevel from workers).
3. Replication paths in Code\client\Services that write state directly (raw bit or field writes) where the Atlas says a real setter must be used (life state, open state, enable/disable, quest stages, alias fills, combat, equipment, perks, globals), or that skip the change flags needed to persist into a save, or that double-fire side effects the catalog says must fire once.
4. Things only a save load relinks (ANSWERS S8 and related) that the mod's network-driven changes need to replay.
5. Concrete open co-op bugs, and whether the Atlas explains them: Helgen tower wall smash sometimes never reaching the wall in a fresh game (MQ101DragonAttack stage 103 skipped, Alduin 32DB7, wall 6CF54; see docs and C:\Tools\skyrim_re\agent\FINDINGS.md 2026-10-01); follower movement re-locked after loading a mid-execution save (MQ101QuestScript.CameraBobStart from player package fragments restarting); manual Journal save renamed as an autosave; Hadvar's horse sideways on the follower; looted armor not shown on the other player (recently patched in InventoryService); host invisible to the follower in the Keep.
6. Cheap wins: Atlas tables or catalog entries that would replace a guess, a hardcoded value or a probe in the mod.

For every finding give: title; severity (crash, desync, visible bug, perf, cleanup); the mod file:line; what the Atlas says with its evidence (doc section or catalog entry, VA and ID); the proposed change in one or two sentences; your confidence; and whether you verified it in the decompiled code yourself. Rank by player impact. Separate "verified against code" from "Atlas claim not yet verified". Be concrete and do not pad: 15 to 40 findings is right. Note any Atlas claim you found to be wrong.
