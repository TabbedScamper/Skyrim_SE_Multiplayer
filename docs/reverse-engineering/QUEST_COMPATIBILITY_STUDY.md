# Skyrim quest compatibility study

## Goal and scope

The goal is not a list of folklore about quests that sometimes break. It is a
versioned inventory of Skyrim quest records, attached scripts, scenes, aliases,
conditions, packages, and world references that assume one local player or one
unreplicated world. That inventory will drive generic synchronization rules,
small data-based compatibility profiles, and reproducible tests.

This document records the method and first MQ101 findings. It does not yet claim
that every quest has been understood or made multiplayer-safe.

## Analysis corpus

For the installed Skyrim SE 1.7.104 data, the local analysis workspace contains:

- the installed `Skyrim.esm` as the authoritative record source;
- 14,026 shipped PEX files extracted from `Skyrim - Misc.bsa` with BSArch;
- local PSC reconstructions produced with Champollion; and
- a focused 50-script MQ101 corpus.

The extracted/decompiled Bethesda material stays outside this repository and is
not redistributed. Repository reports contain only derived identifiers, risk
categories, counts, line numbers, and our own analysis.

`Tools/QuestAudit/Invoke-QuestScriptAudit.ps1` scans a local PSC corpus and emits
JSON/CSV triage. It flags multiplayer-sensitive operations such as local-player
identity, quest writes, scenes, aliases, reference mutation, player controls,
event entry points, dialogue, timers, and save requests. A high score means
"inspect this seam first," not "this quest is proven broken."

`Tools/QuestAudit/QuestRecordAudit` uses the open-source Mutagen library to read
installed plugin records without changing them. It emits derived quest metadata
and joins QUST stages to VMAD fragments, aliases, objectives, and linked scenes.
Dialogue and journal prose are intentionally omitted.

Example:

```powershell
pwsh -File Tools/QuestAudit/Invoke-QuestScriptAudit.ps1 `
  -InputDirectory 'C:\path\to\local\decompiled\scripts' `
  -OutputDirectory 'artifacts\quest-audit'

dotnet run --project Tools/QuestAudit/QuestRecordAudit `
  -- 'C:\path\to\Skyrim.esm' 'artifacts\mq101-record.json' MQ101
```

## Record-level pass

Static script triage is only one layer. For every QUST record, the study must
also extract and correlate:

1. editor ID, form ID, quest type, priority, flags, and start conditions;
2. stages, log entries, objectives, and the exact fragment bound to each stage;
3. reference/location aliases, fill rules, optional/essential flags, and alias
   scripts;
4. dialogue branches/topics and conditions;
5. SCEN records, actors, phases, actions, packages, and start/stop conditions;
6. referenced doors, triggers, furniture, containers, encounters, globals, and
   enable parents; and
7. transitions to other quests and mutually exclusive branches.

The xEdit QUST definition confirms that INDX introduces stages, QOBJ introduces
objectives, ALST/ALLS introduce aliases, and the record VMAD contains quest,
fragment, and alias script attachments. This is the linkage needed to map a
decompiled `Fragment_N` back to the authored stage instead of guessing from
function order.

## Risk taxonomy

| Risk | Typical symptom | Generic treatment |
|---|---|---|
| Local-player condition | Different trigger/stage result per client | Followers propose; leader evaluates and commits |
| Async event echo | Stage or reward runs twice | Transaction ID plus suppression context |
| Alias divergence | NPC/object differs or disappears | Replicate alias fill and mutation in quest snapshot |
| Scene divergence | NPCs speak/move differently | Host scene authority and phase replication |
| Dialogue contention | Two players talk to one NPC | Exclusive conversation lease |
| Reference mutation | Door, corpse, item, or actor differs | Ordered authoritative reference transaction |
| Latent timing | `Wait` resumes in a different world state | Journal continuation or compatibility gate |
| Random/leveled selection | Different target/reward | Choose once on authority and journal the result |
| Player control/camera | Follower stuck, teleported, or hijacked | Per-player presentation command; host-only world effect |
| Save request | One client checkpoints a different world | Replace with coordinated save barrier |
| Branch choice | Party enters incompatible quest paths | One campaign decision, explicit party presentation |

## Initial MQ101 findings

MQ101 is a concentrated example of nearly every high-risk class:

- its quest fragment operates directly on `Game.GetPlayer()`;
- the player alone is mounted to a cart vehicle;
- global chargen, controls, first-person geometry, AI-driven state, restraint,
  movement, inventory, equipment, saves, and RaceMenu are manipulated;
- scene and alias state is started, stopped, moved, enabled, and disabled;
- trigger, enemy, friend, door, hit, combat, death, and item events set stages;
- Dragon Attack uses player-relative and line-of-sight decisions; and
- the Hadvar/Ralof choice starts different successor quests.

The first complete local scan processed all 14,026 shipped PSC reconstructions:
9,139 contained one or more triage patterns, with 309 initially ranked critical,
524 high, 1,245 medium, and 7,061 low. MQ101 ranked first with score 152. These
numbers prioritize review and are not defect counts.

The installed MQ101 record contains 124 stages, 7 objectives, 136 aliases, and
17 linked scenes. Record-to-script correlation establishes the key opening
bindings: RaceMenu at stage 75, chargen exit/autosave at stage 80, player AI
release at stage 140, initial escape controls at stage 160, and full control
restoration at stage 240.

The first read-only `Skyrim.esm` record export completed across 1,811 quests,
5,220 stages, 12,891 aliases, and 1,706 quest-linked scenes. The private derived
JSON is the baseline for joining script scores to actual quest structure and for
generating the main/faction/civil-war/radiant review queues.

The multiplayer design and required guards are specified in
`INTRO_MQ101_MULTIPLAYER.md`.

## Study order

1. Finish MQ101 QUST/SCEN/alias/fragment correlation and instrument its live
   host-control boundary.
2. Trace MQ102A/MQ102B through Riverwood and the first shared save.
3. Audit the main quest chain and its cross-quest scenes/dialogue.
4. Audit faction quest lines, especially mutually exclusive and radiant stages.
5. Audit civil-war world-state swaps and city sieges.
6. Audit Daedric/side quests with unique followers, transformations, prisons,
   dream states, forced travel, or player-only scenes.
7. Audit radiant/story-manager quests as systems, including target selection,
   alias reuse, cooldowns, and reset behavior.
8. Add DLC and installed-mod profiles after the vanilla contract passes.

Each unit advances through: static record/script report, manual semantic review,
compatibility decision, automated two-client simulation, packet-loss/reorder
tests, save/reconnect test, and finally a live two-machine playthrough.
