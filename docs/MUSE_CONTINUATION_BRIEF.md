# Muse continuation brief — authoritative Skyrim co-op research

Continue the research program defined in `docs/MUSE_REVERSE_ENGINEERING_BRIEF.md`.
This is a research-only run. Preserve the dirty working tree and write only under
`docs/reverse-engineering/`.

The phase-one documents already exist. Do not recreate superficial summaries.
Deepen them with code-backed findings that directly support a two-player,
party-leader-authoritative Skyrim Special Edition co-op implementation.

## Required work for this run

1. Fetch or inspect the canonical `tiltedphoques/TiltedEvolution` `dev` branch,
   pin its exact commit and license, and trace these paths end to end:
   - actor ownership transfer;
   - player/NPC death, bleedout, resurrection, and respawn;
   - quest stage and quest-item synchronization;
   - inventory/container/drop identity and mutation;
   - cell/worldspace transitions, calendar, weather, and reset state;
   - disconnect/reconnect and leader loss.
2. Inspect `rfortier/TiltedEvolution-rwf`, especially the experimental quest
   synchronization work and its deduplication/history strategy. Pin the exact
   commit. Clearly distinguish merged upstream code from fork-only experiments.
3. Search for legally available open-source Skyrim multiplayer prior art,
   including Tamriel Online or successors/forks. Inspect source rather than
   descriptions. Record license and reject unverifiable or closed-source claims.
4. Use CommonLibSSE-NG and SKSE source to map Skyrim engine seams for save/load,
   quest stages and aliases, persistent references, cell reset, actor death,
   inventory ExtraData, and dropped references. Every claimed hook must include
   a symbol, source path, Address Library ID, vtable slot, or explicitly marked
   live-probe requirement for runtime 1.7.104.
5. Build a failure-mode matrix for a two-player session: authority owner,
   canonical identity, ordering/idempotency requirement, persistence boundary,
   join-in-progress snapshot need, and likely divergence symptom. Include at
   least death/respawn, quest aliases/scenes, random encounters/leveled lists,
   cell reset clocks, containers, quest items, dropped objects, followers,
   mounts, dialogue, AI packages/combat targets, time/weather, and Papyrus
   latent state.
6. Integrate the existing unattended in-game test bridge
   (`Tools/InGameTests/`) into `LIVE_PROBES.md` and the backlog. Prefer
   non-destructive probes while no human tester is available. Do not mutate or
   overwrite player saves.

## Evidence rules

- Separate verified source facts, inferred design, hypotheses, and unknowns.
- Cite repository URL, exact commit, license, and relevant file/function for
  every reusable technique.
- Do not include Bethesda code or assets in the repository.
- Do not bypass DRM, authentication, or access controls.
- Fix existing mojibake in documents you touch.
- End by updating `INDEX.md` with completed work, remaining blockers, and the
  next three concrete implementation/probe slices.
