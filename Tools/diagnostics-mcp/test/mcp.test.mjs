import assert from "node:assert/strict";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

test("MCP server exposes the diagnostics toolset", async () => {
  const testDirectory = path.dirname(fileURLToPath(import.meta.url));
  const serverPath = path.resolve(testDirectory, "../src/server.mjs");
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [serverPath],
  });
  const client = new Client({ name: "diagnostics-test", version: "0.1.0" });

  try {
    await client.connect(transport);
    const { tools } = await client.listTools();
    const names = tools.map((tool) => tool.name).sort();

    assert.deepEqual(names, [
      "capture_client_bundle",
      "capture_issue",
      "capture_snapshot",
      "client_game_snapshot",
      "client_status",
      "compare_client_snapshots",
      "latest_snapshot",
      "list_bug_markers",
      "mark_bug",
      "player_messages",
      "recent_errors",
      "recent_events",
      "record_client_timeline",
      "run_protocol_tests",
      "send_game_message",
      "session_status",
      "watch_quest",
    ]);

    const response = await client.callTool({ name: "session_status", arguments: {} });
    assert.equal(response.isError, undefined);
    assert.match(response.content[0].text, /STServerOut\.log/);
  } finally {
    await client.close();
  }
});
