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
| 2 | Lockpicking: two players can pick at once and the world freezes | "Lockpicking Menu" was not in the live-menu allow list, so the picker's game paused (and every actor it simulates froze for the others); BusyLock now leases locked doors/containers (kind Lockpicking) | changed |
| 3 | When the follower steps in fire the host is damaged | Isolated `agent\fire_test.ps1` (tundra, 1500 u apart, clean baseline): no host damage with fire on both PCs, on the follower only, on the host only at her copy, or near the host; not reproduced. Needs the real scene (dragon fire, burning effect). Also: a fire placed by one PC exists only there | open (no repro) |
| 4 | Ralof naked on the follower after the chopping block | follower log for 17:28-17:36 lost to rotation (now 64 MB x 6) | open |
| 5 | Tower roof hole: visible on the follower but collision as if intact | | open |
| 6 | Character creation sliders spill over to other players | | open |
| 7 | Follower cannot hold A to drag physics objects (barely moves) | | open |
| 8 | Follower health bar only appears after being revived; want a button prompt and a Skyrim-style progress bar for reviving | | open |
| 9 | Double items on corpses | | open |
| 10 | Player armor flashing in and out | | open |
| 11 | "To Helgen Keep" message repeats continuously inside the keep | The native door activation prompt stays on screen. The door vote held 25FAE for 3 s (17:44:29-32) and replayed the activation; suspect the crosshair target is not cleared on the replay path. Needs a door repro | open |
| 12 | Tails stuck straight out in Helgen Keep until downed/revived | | open |
| 13 | Other players cannot see dropped items | Both installed STServer.ini files had `bEnableItemDrops=false` (the old upstream default; code default is true), so every drop was registered as Local only. Set true on the host install (backups `*.pre-itemdrops`). Two-PC drop test (`agent\drop_test.ps1`): each drop appears on the other PC within 2 s at the same spot | fixed (config) |
| 14 | Remote players' idle animation plays too fast | | open |
| 15 | Mage "magic hands" effect stays active | | open |

Also found while testing: my drop_item test command crashed both games three times (fixed: runs on the game thread, no Papyrus DropObject return); `agent\crash_check.ps1` now reports any crash on either PC from the dumps. A script-placed item (PlaceAtMe) is not shared.

Also: after teleporting both players apart, the follower ended up 150 u from the host (party
gather?), and every checkpoint is inside the intro (no free-roam save for isolated tests).
