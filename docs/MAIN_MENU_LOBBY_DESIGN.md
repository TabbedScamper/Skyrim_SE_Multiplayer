# Main menu and party lobby design (agreed with the owner 2026-10-01)

## Goal

A party-first main menu like a Call of Duty or Fortnite lobby: the party's characters stand in a small 3D room, everyone picks a character and readies up, then the host starts. Single-player and co-op saves are kept apart.

## Main menu buttons

- **Continue -> Play.** Opens two tabs:
  - **Single Player:** only the player's own saves, exactly like vanilla. Co-op checkpoints (`SSC_*.ess`) and saves a guest made in someone else's world are not listed.
  - **Multiplayer:** the party lobby (below).
- **New Game -> New Game Together** when the player is in a party: the host starts a fresh game for the whole party (the existing together start, `start_new_campaign`). Everyone begins in the cart as vanilla's default Nord prisoner and makes their character at the usual point. Alone, New Game stays vanilla.

## Party lobby

- **Party row:** one slot per player, up to 5. Each slot shows that player's chosen character standing idle in the lobby room, with a nameplate (name, level, race), a crown for the host and a Ready marker. Empty slots show "Invite" and open the Steam friend list.
- **No character yet:** the slot shows vanilla's default Nord prisoner (the cart-ride character).
- **Character choice (guests):** pick any of your own characters (from save snapshots). Your slot updates live while browsing. Then press **Ready**. Changing the character clears Ready.
- **Campaigns (host):** co-op checkpoints grouped by campaign, as cards (screenshot, location, date, who played), with Continue, New campaign, rename and delete. More than 12 pages.
- **Start (host):** disabled until every member is Ready ("Waiting for N players").
- **Joining a continued campaign without a character:** the player comes in as the default Nord; offer the character creator once they are in the world.

## New Game Together with a character from another save (owner 2026-10-01)

- In the lobby each player picks **New character** or one of their saved characters, then readies up.
- Everyone rides the cart and goes through the execution as vanilla's default Nord prisoner.
- At the chopping block, when the game opens the character creator (CreatorTogether already manages this moment per player): new-character players get the normal creator; players who brought a character **skip it**. Their snapshot is applied there (appearance, race, name, level, skills, perks, spells, shouts) with the existing CharacterSnapshots apply, and the creator is closed for them the way the harness finishes it, so MQ101 advances normally.
- Their inventory is **not** given yet: they stay a bound prisoner in rags like everyone else.
- **Full inventory at the untying in the Keep:** MQ101 (3372B) stage 240, Fragment_74 (`RemoveItem PrisonerCuffsPlayer`). Restore the snapshot's items and worn state after that point (vanilla strips everything with RemoveAllItems earlier in the intro, so restoring after stage 240 is after the last strip). They still receive the Keep's starter gear.
- Quest items from the other world stay behind (ExcludedQuestItems). Skip the intro's race power grant for a brought character (they already have their spells); check the creator-finish script for that grant.
- Balance is the player's choice: a high-level character in Helgen is allowed.

## Lobby room

**Locations (owner 2026-10-01):** the lobby uses campfire spots from **Interesting Locations** by DrainvolOFF, bundled in the installer with credit (permission terms in docs/THIRD_PARTY.md). The lobby keeps a small list of locations (cell, camera, five posing marks around the fire) and can rotate between them; our own vanilla campfire stays as the fallback. Bundling it changes the required plugin set: both test PCs need it, and checkpoints made before it no longer match the load order.


**A campfire under the night sky** (owner 2026-10-01): a small enclosed cell in our plugin, party members gathered around the fire, crackling fire and ambient night sounds, soft firelight on the characters. Fixed camera, HUD hidden, CEF menu drawn over it. Characters are built from CharacterSnapshot appearance and equipment (the same data a drop-in join uses), with vanilla idles. Loading the room takes a few seconds after the title screen; pressing Continue or Start loads the real save from there.

## Save separation

- Co-op checkpoints listed only in the Multiplayer tab, hidden from Skyrim's own Load menu.
- On guests, engine autosaves, quicksaves and menu saves in the host's world become co-op checkpoints (or are blocked with a short notice) instead of ordinary saves, so a guest's single-player list never holds a copy of the host's world. The host's saves stay normal.
- On leaving, a guest's character progress goes home as a new save in their own world (guest model).

## Stages

1. Menu split (Play with the two tabs, New Game Together), save separation, campaign grouping, 2D cards, ready-up with character choice.
2. 3D lobby room with live character preview.
3. Polish: emotes, camera moves on join, lobby music, and a **live background** (owner 2026-10-01): the lobby is a real outdoor spot, so weather and time of day run live; pick moods on purpose (clear aurora night, light snow, sunrise) through WeatherService and the game clock; a non-hostile dragon on a flying package circling the camp and now and then swooping over the fire; ambient life and fire sound; an optional slow day/night cycle. A **simple lobby** setting (static campfire room) for weaker PCs, since a live world costs about as much as gameplay.

**Candidate spots** (computed from the plugin's own placements and the navmesh, C:\Tools\skyrim_re\agent\lobby\lobby_locations.json, tools find_campfires.py and pose_marks.py beside it): best is cell 0000976F, grid (5,-14), burning campfire with a tent and bedroll behind the party and all five marks on navmesh; next 000099B5, grid (-19,0), campfire with a bench, all marks on navmesh; five more open campfires with all marks on navmesh for rotation. Needs an in-game check for clipping before use.

Order: after the threading work and the 5-player baseline (owner, 2026-10-01).
