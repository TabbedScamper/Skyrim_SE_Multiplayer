import fs from "node:fs/promises";

function unwrapSnapshot(value) {
  let current = value;
  for (let depth = 0; depth < 4 && current && typeof current === "object"; depth += 1) {
    if (current.player && current.session) return current;
    if (current.game) current = current.game;
    else if (current.native) current = current.native;
    else break;
  }
  throw new Error("File does not contain a recognizable game snapshot");
}

function stable(value) {
  return JSON.stringify(value ?? null);
}

function addDifference(differences, path, leader, follower, severity = "error") {
  if (stable(leader) !== stable(follower))
    differences.push({ path, severity, leader, follower });
}

function questMap(snapshot) {
  return new Map((snapshot.quests ?? []).map((quest) => [quest.editorId, quest]));
}

export function compareSnapshots(leaderInput, followerInput) {
  const leader = unwrapSnapshot(leaderInput);
  const follower = unwrapSnapshot(followerInput);
  const differences = [];

  addDifference(differences, "session.online", leader.session?.online, follower.session?.online);
  addDifference(differences, "session.inParty", leader.session?.inParty, follower.session?.inParty);
  addDifference(differences, "session.leaderPlayerId", leader.session?.leaderPlayerId, follower.session?.leaderPlayerId);
  addDifference(differences, "session.memberCount", leader.session?.memberCount, follower.session?.memberCount);

  for (const field of ["cellId", "worldspaceId", "dead", "bleedingOut"])
    addDifference(differences, `player.${field}`, leader.player?.[field], follower.player?.[field]);
  for (const field of ["inCombat", "weaponDrawn", "packageFormId"])
    addDifference(differences, `player.${field}`, leader.player?.[field], follower.player?.[field], "warning");

  addDifference(differences, "menus", leader.menus ?? [], follower.menus ?? [], "warning");

  const leaderQuests = questMap(leader);
  const followerQuests = questMap(follower);
  for (const editorId of new Set([...leaderQuests.keys(), ...followerQuests.keys()])) {
    const left = leaderQuests.get(editorId);
    const right = followerQuests.get(editorId);
    if (!left || !right) {
      differences.push({ path: `quests.${editorId}`, severity: "error", leader: left ?? null, follower: right ?? null });
      continue;
    }
    for (const field of ["currentStage", "state", "enabled", "active", "stopped", "doneStages"])
      addDifference(differences, `quests.${editorId}.${field}`, left[field], right[field]);
  }

  let playerDistance = null;
  const leftPosition = leader.player?.position;
  const rightPosition = follower.player?.position;
  if (Array.isArray(leftPosition) && Array.isArray(rightPosition) && leftPosition.length === 3 && rightPosition.length === 3) {
    playerDistance = Math.sqrt(leftPosition.reduce((sum, value, index) => sum + ((value - rightPosition[index]) ** 2), 0));
  }

  return {
    synchronized: differences.every((entry) => entry.severity !== "error"),
    errorCount: differences.filter((entry) => entry.severity === "error").length,
    warningCount: differences.filter((entry) => entry.severity === "warning").length,
    playerDistance,
    differences,
    lastLeaderQuestEvent: leader.questEvents?.at(-1) ?? null,
    lastFollowerQuestEvent: follower.questEvents?.at(-1) ?? null,
  };
}

export async function compareSnapshotFiles(leaderPath, followerPath) {
  const [leaderText, followerText] = await Promise.all([
    fs.readFile(leaderPath, "utf8"),
    fs.readFile(followerPath, "utf8"),
  ]);
  return compareSnapshots(JSON.parse(leaderText), JSON.parse(followerText));
}
