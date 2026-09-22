# Live probes — minimal, non-destructive, bridge-run

All probes are read-only unless stated; writes use engine functions, never raw
patches. Each probe names its hypothesis, seam, and pass criterion.

- **P1 — Address sanity.** Read UI3DSceneManager singleton via ID 403560 and
  Mist FOV at +0x100 through the bridge; compare with F9 report values.
  Pass: bridge reads == F9 reads. Gates every later write probe.
- **P2 — Camera function path.** Reapply `MistMenu::cameraFOV` via
  `SetCameraFOV` (ID 52742) after a window resize; before/after F9 reports.
  Pass: title-model projection matches new aspect. (Already the recorded
  direction; this probe is its acceptance test.)
- **P3 — VM load behavior.** Log `IsCompletelyFrozen`/overstress state + frame
  script time while spawning script load. Pass: thresholds observed and
  documented; informs host-load design.
- **P4 — Focus/pause.** Alt-tab with menus open/closed; record VM update,
  menu stack, and simulation advance per case. Pass: pause matrix documented
  (what stops, what continues).
- **P5 — Quest convergence (two-client, needs bridge on both).** Advance a
  quest stage on leader; snapshot follower journal/aliases. Pass or record
  exact divergence fields.
- **P6 — Cell-transition authority.** Leader + follower cross a cell boundary
  in both orders; snapshot resident references. Pass: identical sets.
- **P7 — Shared kill.** Kill a shared actor from follower client; compare
  dead/alive + corpse identity + respawn timing across peers.
- **P8 — Loot race.** Concurrent same-container loot + drop/pickup; compare
  inventories incl. ExtraData flags against leader ledger.

Probe harness seam: extend the existing F9 snapshot + named-pipe JSON bridge
(Skyrim MCP pattern) with one command per probe; store reports under the
runtime's `debug-feedback` directory, never in-repo.
