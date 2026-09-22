import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import {
  getLatestSnapshot,
  getMarkers,
  getPlayerMessages,
  getRecentErrors,
  getRecentEvents,
  getSessionStatus,
  markBug,
  queueCommand,
  requestSnapshot,
} from "./diagnostics.mjs";
import {
  captureClientBundle,
  getGameSnapshot,
  getNativeStatus,
  recordClientTimeline,
  watchQuest,
} from "./native-client.mjs";
import { compareSnapshotFiles } from "./snapshot-diff.mjs";
import { runProtocolTests } from "./protocol-tests.mjs";
import { pullLatestFollowerCapture } from "./remote-client.mjs";

const server = new McpServer(
  { name: "skyrim-seamless-diagnostics", version: "0.1.0" },
  {
    instructions:
      "Read live Skyrim Together state before diagnosing a gameplay bug. Start with session_status, then recent_errors or recent_events. Use mark_bug when the players identify the moment a problem occurs. send_game_message writes only a visible in-game diagnostic message; it cannot execute console commands. State-changing debug controls are intentionally unavailable.",
  },
);

function result(value) {
  return {
    content: [{ type: "text", text: JSON.stringify(value, null, 2) }],
  };
}

server.registerTool(
  "session_status",
  {
    description: "Show configured Skyrim Together logs and bridge health.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await getSessionStatus()),
);

server.registerTool(
  "client_status",
  {
    description:
      "Check the local Skyrim client bridge protocol and supported live commands.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await getNativeStatus()),
);

server.registerTool(
  "client_game_snapshot",
  {
    description:
      "Read the latest game-thread snapshot: player, controls, camera, menus, party, watched quests, and quest event journal.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await getGameSnapshot()),
);

server.registerTool(
  "watch_quest",
  {
    description:
      "Add a quest editor ID to the live client snapshot watch list. MQ101 is watched by default.",
    inputSchema: z.object({ editor_id: z.string().min(1).max(128) }),
    annotations: { readOnlyHint: false },
  },
  async ({ editor_id }) => result(await watchQuest(editor_id)),
);

server.registerTool(
  "capture_client_bundle",
  {
    description:
      "Capture a local Skyrim screenshot and matching game-thread JSON state bundle.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await captureClientBundle()),
);

server.registerTool(
  "record_client_timeline",
  {
    description:
      "Record repeated game-thread snapshots to a timestamped JSON artifact for diagnosing state transitions that a single capture misses.",
    inputSchema: z.object({
      duration_ms: z.number().int().min(500).max(30000).default(5000),
      interval_ms: z.number().int().min(100).max(2000).default(100),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ duration_ms, interval_ms }) =>
    result(await recordClientTimeline({ durationMs: duration_ms, intervalMs: interval_ms })),
);

server.registerTool(
  "capture_issue",
  {
    description:
      "Capture one correlated issue bundle: a bug marker, local screenshot and game state, authoritative server snapshot, and recent errors.",
    inputSchema: z.object({ description: z.string().min(1).max(500) }),
    annotations: { readOnlyHint: false },
  },
  async ({ description }) => {
    const marker = await markBug(description);
    const [client, serverSnapshot, errors] = await Promise.allSettled([
      captureClientBundle(),
      requestSnapshot({ reason: `bug ${marker.id}: ${description}`, waitMs: 3000 }),
      getRecentErrors(50),
    ]);
    const unwrap = (entry) =>
      entry.status === "fulfilled"
        ? { ok: true, value: entry.value }
        : { ok: false, error: entry.reason?.message ?? String(entry.reason) };
    return result({
      marker,
      client: unwrap(client),
      server: unwrap(serverSnapshot),
      recent_errors: unwrap(errors),
    });
  },
);

server.registerTool(
  "compare_client_snapshots",
  {
    description:
      "Compare saved leader and follower game-state JSON files and report meaningful multiplayer divergence while ignoring expected local identity differences.",
    inputSchema: z.object({
      leader_path: z.string().min(1),
      follower_path: z.string().min(1),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ leader_path, follower_path }) =>
    result(await compareSnapshotFiles(leader_path, follower_path)),
);

server.registerTool(
  "pull_follower_capture",
  {
    description:
      "Read the follower PC's newest F10 screenshot, note, and game-state bundle over pinned key-based SSH and mirror it into the local diagnostics workspace.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await pullLatestFollowerCapture()),
);

server.registerTool(
  "run_protocol_tests",
  {
    description:
      "Run two headless production-protocol clients against an isolated dedicated server and report join convergence plus known authority gaps.",
    inputSchema: z.object({
      port: z.number().int().min(1024).max(65535).default(12578),
      fail_on_known_gaps: z.boolean().default(false),
    }),
    annotations: { readOnlyHint: false },
  },
  async ({ port, fail_on_known_gaps }) =>
    result(await runProtocolTests({ port, failOnKnownGaps: fail_on_known_gaps })),
);

server.registerTool(
  "recent_events",
  {
    description:
      "Read and correlate recent structured entries from server and client logs.",
    inputSchema: z.object({
      limit: z.number().int().min(1).max(500).default(100),
      severity: z
        .enum(["trace", "debug", "info", "warning", "error", "critical"])
        .optional(),
      source: z.enum(["server", "leader", "leader_previous"]).optional(),
      pattern: z.string().max(300).optional(),
    }),
    annotations: { readOnlyHint: true },
  },
  async (input) => result(await getRecentEvents(input)),
);

server.registerTool(
  "recent_errors",
  {
    description: "Return recent warnings, errors, and crashes from all logs.",
    inputSchema: z.object({
      limit: z.number().int().min(1).max(300).default(100),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ limit }) => result(await getRecentErrors(limit)),
);

server.registerTool(
  "mark_bug",
  {
    description:
      "Record the exact time and player description of an observed gameplay bug.",
    inputSchema: z.object({ description: z.string().min(1).max(500) }),
    annotations: { readOnlyHint: false },
  },
  async ({ description }) => result(await markBug(description)),
);

server.registerTool(
  "list_bug_markers",
  {
    description: "List the most recent player-created bug markers.",
    inputSchema: z.object({
      limit: z.number().int().min(1).max(200).default(50),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ limit }) => result(await getMarkers(limit)),
);

server.registerTool(
  "player_messages",
  {
    description:
      "Read recent reports entered by either player with !codex in Skyrim Together chat.",
    inputSchema: z.object({
      limit: z.number().int().min(1).max(200).default(50),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ limit }) => result(await getPlayerMessages(limit)),
);

server.registerTool(
  "capture_snapshot",
  {
    description:
      "Ask the running STR server for an authoritative party and world snapshot.",
    inputSchema: z.object({
      reason: z.string().max(300).default("manual"),
      wait_ms: z.number().int().min(0).max(10000).default(2000),
    }),
    annotations: { readOnlyHint: true },
  },
  async ({ reason, wait_ms }) =>
    result(await requestSnapshot({ reason, waitMs: wait_ms })),
);

server.registerTool(
  "latest_snapshot",
  {
    description: "Read the latest authoritative snapshot emitted by the server.",
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  },
  async () => result(await getLatestSnapshot()),
);

server.registerTool(
  "send_game_message",
  {
    description:
      "Queue a visible diagnostic message from Codex to connected players. This cannot run game or server console commands.",
    inputSchema: z.object({ message: z.string().min(1).max(400) }),
    annotations: { readOnlyHint: false },
  },
  async ({ message }) =>
    result({ status: "queued", request_id: await queueCommand("message", message) }),
);

const transport = new StdioServerTransport();
await server.connect(transport);
