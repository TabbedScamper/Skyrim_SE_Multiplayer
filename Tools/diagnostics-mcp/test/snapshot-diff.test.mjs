import assert from "node:assert/strict";
import test from "node:test";
import { compareSnapshots } from "../src/snapshot-diff.mjs";

function snapshot({ leader, stage = 10, dead = false, position = [0, 0, 0] }) {
  return {
    session: { online: true, inParty: true, leader, leaderPlayerId: 7, memberCount: 2 },
    player: { cellId: 1, worldspaceId: 2, dead, bleedingOut: false, inCombat: false, weaponDrawn: false, packageFormId: 0, position },
    menus: [],
    quests: [{ editorId: "MQ101", currentStage: stage, state: 1, enabled: true, active: true, stopped: false, doneStages: [5, stage] }],
    questEvents: [],
  };
}

test("snapshot comparison ignores expected per-client identity and measures distance", () => {
  const comparison = compareSnapshots(snapshot({ leader: true }), snapshot({ leader: false, position: [3, 4, 0] }));
  assert.equal(comparison.synchronized, true);
  assert.equal(comparison.errorCount, 0);
  assert.equal(comparison.playerDistance, 5);
});

test("snapshot comparison identifies quest and lifecycle divergence", () => {
  const comparison = compareSnapshots(snapshot({ leader: true, stage: 20 }), snapshot({ leader: false, stage: 10, dead: true }));
  assert.equal(comparison.synchronized, false);
  assert.ok(comparison.differences.some((entry) => entry.path === "player.dead"));
  assert.ok(comparison.differences.some((entry) => entry.path === "quests.MQ101.currentStage"));
});
