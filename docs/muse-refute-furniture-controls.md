Refute these two changes. Read-only: do not edit files, build, or run the game. Put your full findings in your final answer.

Repo: C:\Users\mwalt\SkyrimSeamlessCoop-ui (Skyrim SE 1.7.104 co-op mod; client DLL in the game process). The whole exe is decompiled offline: C:\Tools\skyrim_re\sk.cmd fn|callers|callees|id (see docs\reverse-engineering\EXE_CORPUS.md). Engine research: C:\Users\mwalt\SkyrimAtlas\systems\animation-graph-and-furniture.md, ai-process.md, and for-coop\ANSWERS.md (A1-A6, P9, H2, H3, S8).

Change 1: Code\client\Services\FurnitureGraphLink.h and Code\client\Services\Generic\FurnitureGraphLink.cpp (registered in Code\client\World.cpp; probe command furniture_links in GameTestService.cpp).
Once a second on the game thread, for every high and middle-high actor whose MiddleHigh+0x208 furniture handle resolves to a FURN reference with an animation graph manager, it calls N63364 (0x140BC0E60 -> 0x140BC1D80) with flag 0 to add the furniture's manager as a dependent of the actor's manager, if missing. It also hooks N63372 (0x140BC1600) to log "Open" forwards. Live result: Alduin (32DB7) in the Helgen tower wall (6CF54) was unlinked on host and follower after every load and in two full runs; after relinking, Alduin's Open reached the wall and MQ101DragonAttack stage 103 fired.
Claims:
1a. The relink cannot crash or corrupt: manager lifetimes (GetBSAnimationGraph add-refs, Release), locking (N63364 takes +0xA8), the unlocked reads in HasDependent/DependentCount, actors or furniture deleted or unloaded during the call, middleProcess null, handle reuse.
1b. It never adds a link the engine would not have: actors that are leaving furniture, mounted actors (H2: Actor+0x1F0 vehicle handle makes N39912 skip the link on purpose), furniture whose graph should not follow the actor, an actor in non-animated furniture, the player. Does the engine ever remove the dependent again (furniture exit), so a link we added is cleaned up? Find the removal path and whether it matches by pointer.
1c. Flag 0 versus the sit path's flag 1: what does flag 1 do (0x140BC2740 with the 4th argument), and does skipping it break anything?
1d. Cost: once a second over the high and middle-high lists with 5 players and a busy city.
1e. The N63372 hook is safe on every thread that calls it.

Change 2: Code\client\Services\Generic\PartyService.cpp, PlayerControlSync::Update, the branch for stored != 0x80000000.
A follower whose ControlMap has a saved mask (+0x124) that differs from the host's released controls takes the host's controls into the saved mask too (ToggleControls 68545 with alsoStored=true) after 3 s, when the host is free, no menu/dialogue/scene is up (gated earlier in Update) and the player uses no furniture (MiddleHigh+0x208 == 0).
Claims:
2a. It cannot fight a legitimate local owner of the saved mask: list every engine and script path that stores controls (68546 callers, ToggleControls with alsoStored) and whether each is excluded by the gates.
2b. It cannot unlock a player who should stay locked: bleedout, death, werewolf/vampire lord transformation, mounted, kill-cam, fast travel, sleeping/waiting, crafting, a follower-local quest scene that the host does not run.
2c. Whether this is the right fix for a follower loaded from a mid-intro checkpoint, and what else after a load can leave stale controls.

For each claim: REFUTED (concrete failing case and file:line or VA), HOLDS (with evidence), or UNVERIFIED. Rank real defects by severity.
