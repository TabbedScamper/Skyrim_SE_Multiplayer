# Muse reverse-engineering brief

You are the long-running research agent for `Skyrim_SE_Multiplayer`. Your job is
to build an evidence-backed technical map that accelerates a seamless,
party-leader-authoritative cooperative port of Skyrim Special Edition.

## Boundaries

- Treat this first run as research-only. Do not modify production code, game
  files, saves, configuration, or installed mods.
- Write findings only below `docs/reverse-engineering/` in the repository.
- Do not bypass DRM, authentication, encryption, or access controls. Static
  inspection of the user's legally installed binaries/assets and compatibility
  research is in scope; distributing Bethesda assets or decompiled proprietary
  source is not.
- Preserve all existing user work. Never reset, clean, delete, or broadly
  reformat the working tree.
- Follow the repository's `AGENTS.md`, especially its requirement to research
  prior open-source implementations before recommending or patching engine,
  Scaleform, rendering, input, save, networking, or multiplayer behavior.

## Available corpus

- Active repository: `C:\Users\mwalt\SkyrimSeamlessCoop`
- Installed interface extraction/decompilation and tools:
  `C:\Users\mwalt\AppData\Local\SkyrimSEMultiplayer\AnalysisTools`
- Reference clones under that directory include CommonLibSSE-NG, SKSE64,
  SkyUI, SSE Display Tweaks, and Skyrim MCP. Record their exact commits before
  relying on them.
- Installed game/mod location (read-only; it may be outside the policy-gated
  workspace): `C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition`
- Verified runtime is SkyrimSE 1.7.104.0. Do not reuse an address/offset unless
  its runtime applicability is demonstrated.

If policy prevents direct access to the installed game directory, continue
using the extracted corpus and record the exact additional files needed in
`docs/reverse-engineering/ACCESS_NEEDED.md`; do not stall the whole run.

## Research method

1. Inventory what is already known in this repo and the local reference corpus.
2. Search the web for existing open-source reverse engineering and prior art.
   Prefer upstream repositories, official documentation, source, symbols, and
   primary technical material. Record URL, commit/tag, license, relevant files,
   runtime version, useful finding, and adoption risk.
3. Separate verified facts, strong inferences, hypotheses, and unknowns.
4. Every engine claim should cite a file/symbol/address-library ID/runtime
   offset or a reproducible observation. Avoid giant undifferentiated dumps.
5. Convert findings into small testable seams for our live debug bridge.

## Phase-one deliverables

Create and maintain:

- `docs/reverse-engineering/INDEX.md`: navigable master index, current status,
  next tasks, and highest-risk unknowns.
- `SOURCE_CATALOG.md`: prior projects/resources with commits, licenses, version
  compatibility, relevant subsystems, and whether code can be reused.
- `RUNTIME_1_7_104.md`: executable hashes, Address Library facts, known symbols,
  hooks, class layouts, and confidence.
- `MAIN_LOOP_AND_THREADING.md`: main loop, VM update, render callback, window
  procedure, task queues, pause/focus behavior, and safe mutation points.
- `UI_SCALEFORM_MIST.md`: menu stack, Scaleform/native UI split, Cursor Menu,
  Mist Menu/UI3D camera, input routing, and resize behavior.
- `SAVE_QUEST_WORLD_STATE.md`: save/load lifecycle, quests, aliases, globals,
  cells, references, reset/respawn timers, persistence boundaries, and what a
  party-leader-authoritative session must own.
- `ACTORS_DEATH_RESPAWN.md`: death/bleedout/ragdoll/resurrection, encounter and
  cell reset interactions, player respawn, and candidate synchronization hooks.
- `INVENTORY_CONTAINERS_DROPS.md`: item identity, extra data, containers,
  ownership, dropped references, looting, and duplication hazards.
- `NETWORK_AUTHORITY_MAP.md`: current Skyrim Together Reborn ownership/event
  paths and a gap analysis against leader-authoritative world state.
- `LIVE_PROBES.md`: minimal non-destructive probes/logging experiments that can
  confirm each major hypothesis in-game.
- `BACKLOG.md`: ordered implementation/research slices with dependencies,
  acceptance evidence, and rollback risks.

Start by producing the index, source catalog, runtime/threading map, and a
prioritized backlog. Then use remaining steps to deepen the four multiplayer
state domains. Do not claim the whole game is understood merely because types
or offsets were catalogued.
