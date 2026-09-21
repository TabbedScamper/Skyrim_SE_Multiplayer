import crypto from "node:crypto";
import fs from "node:fs/promises";
import {
  commandFile,
  configuredLogs,
  markerFile,
  runtimeDirectory,
  telemetryFile,
} from "./config.mjs";
import {
  appendLine,
  describeFile,
  parseLogText,
  readJsonLines,
  readTail,
  sanitizeCommandField,
} from "./files.mjs";

const sleep = (milliseconds) =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

function newestFirst(left, right) {
  return String(right.timestamp).localeCompare(String(left.timestamp));
}

export async function getSessionStatus() {
  const logs = await Promise.all(
    configuredLogs.map(async (entry) => ({
      source: entry.source,
      ...(await describeFile(entry.path)),
    })),
  );

  return {
    observed_at: new Date().toISOString(),
    runtime_directory: runtimeDirectory,
    command_mailbox: await describeFile(commandFile),
    telemetry: await describeFile(telemetryFile),
    logs,
  };
}

export async function getRecentEvents({
  limit = 100,
  severity,
  source,
  pattern,
} = {}) {
  const expression = pattern ? new RegExp(pattern, "i") : null;
  const selectedLogs = configuredLogs.filter(
    (entry) => !source || entry.source === source,
  );
  const batches = await Promise.all(
    selectedLogs.map(async (entry) =>
      parseLogText(await readTail(entry.path), entry.source),
    ),
  );

  return batches
    .flat()
    .filter((entry) => !severity || entry.severity === severity)
    .filter((entry) => !expression || expression.test(entry.message))
    .sort(newestFirst)
    .slice(0, limit);
}

export async function getRecentErrors(limit = 100) {
  const events = await getRecentEvents({ limit: Math.max(limit * 4, 200) });
  return events
    .filter((entry) =>
      ["warning", "error", "critical"].includes(entry.severity),
    )
    .slice(0, limit);
}

export async function queueCommand(kind, value = "") {
  const requestId = crypto.randomUUID();
  const line = [
    kind,
    requestId,
    Date.now(),
    sanitizeCommandField(value),
  ].join("\t");
  await appendLine(commandFile, line);
  return requestId;
}

export async function markBug(description) {
  const id = crypto.randomUUID();
  const marker = {
    type: "bug_marker",
    id,
    timestamp: new Date().toISOString(),
    description: sanitizeCommandField(description),
  };
  await appendLine(markerFile, JSON.stringify(marker));
  await appendLine(
    commandFile,
    ["mark", id, Date.now(), marker.description].join("\t"),
  );
  return marker;
}

export async function requestSnapshot({ reason = "manual", waitMs = 2000 } = {}) {
  const requestId = await queueCommand("snapshot", reason);
  const deadline = Date.now() + waitMs;

  while (Date.now() < deadline) {
    const snapshots = await readJsonLines(telemetryFile);
    const snapshot = snapshots.find(
      (entry) =>
        entry.type === "snapshot" && entry.request_id === requestId,
    );
    if (snapshot) return { status: "complete", snapshot };
    await sleep(100);
  }

  return {
    status: "queued",
    request_id: requestId,
    message:
      "The server did not answer before the MCP timeout. The command remains queued.",
  };
}

export async function getLatestSnapshot() {
  const entries = await readJsonLines(telemetryFile);
  return (
    entries.filter((entry) => entry.type === "snapshot").at(-1) ?? null
  );
}

export async function getPlayerMessages(limit = 50) {
  const entries = await readJsonLines(telemetryFile);
  return entries
    .filter((entry) => entry.type === "player_message")
    .slice(-limit)
    .reverse();
}

export async function getMarkers(limit = 50) {
  return (await readJsonLines(markerFile)).slice(-limit).reverse();
}

export async function clearRuntimeForTests() {
  await fs.rm(runtimeDirectory, { recursive: true, force: true });
}
