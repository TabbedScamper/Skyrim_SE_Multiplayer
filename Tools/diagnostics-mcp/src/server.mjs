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
