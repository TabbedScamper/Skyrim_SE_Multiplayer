import fs from "node:fs";
import path from "node:path";

const scenarioPath = process.argv[2];
if (!scenarioPath) {
  process.stderr.write("usage: node player-lifecycle-model.mjs <scenario.json>\n");
  process.exit(1);
}

const scenario = JSON.parse(fs.readFileSync(scenarioPath, "utf8"));
const reviveRadius = Number(scenario.initialState?.reviveRadius ?? 512);
const players = new Map();
const transactionEffects = new Map();
const events = [];

for (const participant of scenario.participants) {
  players.set(participant.id, {
    connected: true,
    state: "alive",
    cell: null,
    position: [0, 0, 0],
    inventory: new Map(),
    downEvent: null,
  });
}

function distance(left, right) {
  return Math.hypot(
    left.position[0] - right.position[0],
    left.position[1] - right.position[1],
    left.position[2] - right.position[2],
  );
}

function isEligibleReviver(targetId, target) {
  for (const [id, candidate] of players) {
    if (id === targetId || !candidate.connected || candidate.state !== "alive") continue;
    if (candidate.cell !== target.cell) continue;
    if (distance(candidate, target) <= reviveRadius) return true;
  }
  return false;
}

function applyOnce(transaction, effect) {
  if (!transaction) throw new Error("state-changing action is missing data.transaction");
  if (transactionEffects.has(transaction)) {
    events.push({ type: "transaction.duplicateIgnored", transaction });
    return false;
  }
  effect();
  transactionEffects.set(transaction, 1);
  return true;
}

function applyAction(action) {
  const actor = players.get(action.actor);
  const data = action.data ?? {};
  if (!actor) throw new Error(`unknown actor '${action.actor}'`);

  switch (action.type) {
    case "session.create":
    case "session.join":
    case "session.reconnect":
      actor.connected = true;
      events.push({ at: action.at, type: action.type, actor: action.actor });
      return;
    case "session.disconnect":
      actor.connected = false;
      events.push({ at: action.at, type: action.type, actor: action.actor });
      return;
    case "player.setPosition":
      actor.cell = String(data.cell);
      actor.position = data.position.map(Number);
      events.push({ at: action.at, type: action.type, actor: action.actor, cell: actor.cell });
      return;
    case "inventory.grant":
      applyOnce(String(data.transaction), () => {
        const item = String(data.item);
        actor.inventory.set(item, (actor.inventory.get(item) ?? 0) + Number(data.quantity));
        events.push({ at: action.at, type: action.type, actor: action.actor, item, quantity: Number(data.quantity) });
      });
      return;
    case "player.lethalDamage":
      applyOnce(String(data.transaction), () => {
        if (actor.state !== "alive") throw new Error(`lethal damage requires '${action.actor}' to be alive`);
        const canRevive = isEligibleReviver(action.actor, actor);
        actor.state = canRevive ? "downed" : "dead";
        actor.downEvent = canRevive ? String(data.transaction) : null;
        events.push({ at: action.at, type: "player.lifecycleChanged", actor: action.actor, state: actor.state });
      });
      return;
    case "player.revive": {
      const target = players.get(String(data.target));
      applyOnce(String(data.transaction), () => {
        if (!actor.connected || actor.state !== "alive") throw new Error("reviver is not eligible");
        if (!target || target.state !== "downed") throw new Error("revive target is not downed");
        if (actor.cell !== target.cell || distance(actor, target) > reviveRadius) throw new Error("revive target is out of range");
        target.state = "alive";
        target.downEvent = null;
        events.push({ at: action.at, type: "player.lifecycleChanged", actor: String(data.target), state: "alive" });
      });
      return;
    }
    default:
      throw new Error(`unsupported lifecycle action '${action.type}'`);
  }
}

function evaluate(assertion) {
  const data = assertion.data ?? {};
  switch (assertion.type) {
    case "player.stateEquals":
      return players.get(String(data.participant))?.state === String(data.state);
    case "inventory.quantityEquals":
      return (players.get(String(data.participant))?.inventory.get(String(data.item)) ?? 0) === Number(data.quantity);
    case "transaction.effectCountEquals":
      return (transactionEffects.get(String(data.transaction)) ?? 0) === Number(data.count);
    default:
      throw new Error(`unsupported lifecycle assertion '${assertion.type}'`);
  }
}

const pendingAssertions = scenario.assertions.map((assertion, index) => ({ assertion, index }));
const assertionResults = new Array(pendingAssertions.length);
const actions = [...scenario.actions].sort((a, b) => a.at - b.at);

for (const action of actions) {
  applyAction(action);
  for (const pending of pendingAssertions) {
    if (assertionResults[pending.index] || Number(pending.assertion.after ?? Infinity) !== Number(action.at)) continue;
    assertionResults[pending.index] = {
      type: pending.assertion.type,
      after: Number(pending.assertion.after),
      passed: evaluate(pending.assertion),
    };
  }
}

for (const pending of pendingAssertions) {
  if (assertionResults[pending.index]) continue;
  assertionResults[pending.index] = {
    type: pending.assertion.type,
    after: pending.assertion.after ?? null,
    passed: evaluate(pending.assertion),
  };
}

const normalizedPlayers = Object.fromEntries([...players.entries()].map(([id, player]) => [id, {
  connected: player.connected,
  state: player.state,
  cell: player.cell,
  position: player.position,
  inventory: Object.fromEntries([...player.inventory.entries()].sort()),
}]));
const passed = assertionResults.every((result) => result.passed);
process.stdout.write(`${JSON.stringify({
  scenario: scenario.name,
  passed,
  detail: passed ? "player lifecycle invariants held" : "player lifecycle invariant failed",
  assertionResults,
  events,
  finalState: { players: normalizedPlayers },
})}\n`);
process.exit(passed ? 0 : 1);
