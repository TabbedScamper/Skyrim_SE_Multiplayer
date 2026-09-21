# Skyrim Together live diagnostics

This bridge gives Codex a read-only view of Skyrim Together logs and
authoritative server snapshots, plus two deliberately narrow write operations:
visible in-game messages and timestamped bug markers. It does not expose the
server console or arbitrary command execution.

## Player workflow

1. Run the custom `SkyrimTogetherServer.exe` normally.
2. Connect both players and form a party.
3. When a problem occurs, open Skyrim Together chat and enter:

   ```text
   !codex follower died and the bandits respawned
   ```

4. The server replies with a `[Codex]` confirmation and records the message,
   players, party leadership, quest stages, loaded actors, death state,
   ownership, cells, and positions in `runtime/diagnostics/telemetry.jsonl`.
5. Ask Codex to inspect the latest player report or capture another snapshot.

The MCP records events continuously, but it cannot independently wake or start
a Codex conversation. Keep a Codex session open while live iteration is needed.

## MCP tools

- `session_status`, `recent_events`, and `recent_errors` inspect bridge health
  and logs.
- `player_messages` reads reports sent with `!codex`.
- `capture_snapshot` and `latest_snapshot` inspect authoritative server state.
- `mark_bug` and `list_bug_markers` correlate a report with telemetry.
- `send_game_message` sends a visible `[Codex]` chat message to both players.

## Configuration

The server bridge is disabled unless its config contains an absolute directory:

```ini
[Diagnostics]
sDirectory=C:\Users\mwalt\SkyrimSeamlessCoop\runtime\diagnostics
```

Install dependencies and test the MCP with:

```powershell
npm install
npm test
```

Codex must be restarted after adding the STDIO MCP so its tool inventory is
refreshed.
