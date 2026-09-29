# Playthrough 2026-09-28: Helgen to the cave exit (host + follower)

Owner report after the first full two-player run of the intro. Logs: `C:\Tools\skyrim_re\playthrough-20260928`
(host 16:49-18:02 complete; follower only 17:36-18:01, the rotation kept 3 x 5 MB). MQ101 timeline from the host
log: stage 0 at 17:31:50, execution stages 20-37 at 17:34-17:35, dragon attack 45-70 at 17:35:28-17:36:00, keep
80-100 at 17:39-17:41, cave exit and crash 18:02:24.

Status: **fixed** = changed and verified in a two-PC run; **changed** = code changed, verification pending;
**open** = not yet addressed.

| # | Report | Finding | Status |
|---|---|---|---|
| 0 | Host crashed at the cave exit when the follower left (Survival prompt time) | AIProcess pending-door activation (39408, 0x1406F6E40) wrote middleHigh+0x470 after ActivateRef unloaded Ralof into the exterior the host had not loaded; dump: rax=0 at 0x1406F6ED7. Replaced by the same steps with a null re-check | changed |
| 1 | Survival Mode prompt keeps popping up; want it to work in co-op later | Survival_MainScript OnUpdate prompts per PC unless Survival_PlayerHasBeenPrompted (esl 0x0E8DF) is 1. Co-op sessions now set it; the settings toggle still works. Party-wide mode: open | fixed (prompt) |
| 2 | Lockpicking: two players can pick at once and the world freezes | "Lockpicking Menu" was not in the live-menu allow list, so the picker's game paused (and every actor it simulates froze for the others); BusyLock now leases locked doors/containers (kind Lockpicking; 1.5 s if the minigame never opens). Reviewed (Muse); in-game check pending | changed |
| 3 | When the follower steps in fire the host is damaged | Owner: it happened where #5 left her outside the intact inn roof while the host's inn was broken and burning. Isolated `agent\fire_test.ps1`: no host damage in any placement, so the leak needs her copy inside the host's burning inn. Instrumented: every negative health effect on a player (local or copy) logs target, caster and spell. Also: a fire placed by one PC exists only there | open (instrumented) |
| 4 | Ralof naked on the follower after the chopping block | follower log for 17:28-17:36 lost to rotation (now 64 MB x 6) | open |
| 5 | Tower roof hole: visible on the follower but collision as if intact (she stood on the "intact" inn roof; the host jumped in) | MQ101DragonAttack (D0593) swaps collision markers (CollisionInnMarker EB625 parents 20 collision boxes around the inn; CollisionInnBackside F8213, CollisionA/B/C, blockers) by EnableNoWait/DisableNoWait on the host. The follower applied them 17:41-17:44, minutes after the stages (17:35:47+), most in one burst at the 17:44:33 checkpoint. Pipeline test (`agent\ws_test.ps1`): a live host enable reaches and applies on the follower in <0.5 s, so the playthrough delay is elsewhere (capture paused or queued). Instrumented: live publishes, receives, per-cell baseline summaries and queue depths are now logged | open (instrumented) |
| 6 | Character creation sliders spill over to other players | | open |
| 7 | Follower cannot hold A to drag physics objects (barely moves) | | open |
| 8 | Follower health bar only appears after being revived; want a button prompt and a Skyrim-style progress bar for reviving | | open |
| 9 | Double items on corpses | The inventory sync sent base-container leveled lists (LVLI entries, not items); RemoveAllItems cannot remove them on the copy, so each list ended up twice and resolved twice as loot. `agent\loot_test.ps1`: the follower copy of a bandit had LootBanditRandom/LItemBanditWeapon1H/LootGoldChange25 at 2 (host 1). Leveled lists are no longer read or applied; rerun: identical inventories on both PCs before and after death | fixed |
| 10 | Player armor flashing in and out | | open |
| 11 | "To Helgen Keep" message repeats continuously inside the keep | The native door activation prompt stays on screen. The door vote held 25FAE for 3 s (17:44:29-32) and replayed the activation; suspect the crosshair target is not cleared on the replay path. Needs a door repro | open |
| 12 | Tails stuck straight out in Helgen Keep until downed/revived | | open |
| 13 | Other players cannot see dropped items | Both installed STServer.ini files had `bEnableItemDrops=false` (the old upstream default; code default is true), so every drop was registered as Local only. Set true on the host install (backups `*.pre-itemdrops`). Two-PC drop test (`agent\drop_test.ps1`): each drop appears on the other PC within 2 s at the same spot | fixed (config) |
| 14 | Remote players' idle animation plays too fast (owner: probably a side effect of the host crash; parked) | Not reproduced for standing player copies: `agent\idle_rate.ps1` shows the copy is not graph-updated at all (it follows the streamed pose); `agent\idle_trace.ps1` head-bone motion equal on owner and copy (1.9/2.0 and 2.0/1.8 u/s). Need which characters (players or NPC companions) and when | open (no repro) |
| 15 | Mage "magic hands" effect stays active | | open |

Also found while testing: my drop_item test command crashed both games three times (fixed: runs on the game thread, no Papyrus DropObject return); `agent\crash_check.ps1` now reports any crash on either PC from the dumps. A script-placed item (PlaceAtMe) is not shared.

Also: after teleporting both players apart, the follower ended up 150 u from the host (party
gather?), and every checkpoint is inside the intro (no free-roam save for isolated tests).
