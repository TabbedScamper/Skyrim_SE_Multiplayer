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
- `client_status` checks the local native bridge and its protocol version.
- `client_game_snapshot` reads player, control, camera, menu, party, watched
  quest, and recent quest-event state sampled on Skyrim's game thread.
- `watch_quest` adds another quest editor ID to that live snapshot; MQ101 is
  always watched from startup.
- `capture_client_bundle` saves a screenshot and its matching game-state JSON.
- `record_client_timeline` samples game-thread state for up to 30 seconds and
  saves the full transition timeline as a JSON artifact.
- `capture_issue` is the fast path for a reported failure: it creates one bug
  marker and correlates a local screenshot/state bundle, authoritative server
  snapshot, and recent errors in one call. Partial results are retained when a
  client or server is unavailable.
- `compare_client_snapshots` compares leader/follower `.game.json` artifacts
  semantically and flags party, cell, lifecycle, menu, and watched-quest
  divergence while ignoring expected per-client identity differences.
- `run_protocol_tests` launches an isolated dedicated server and two headless
  production-protocol clients. It currently checks join/party convergence and
  audits whether a follower can improperly author quest progress.

F10 problem reports now write three matching files under `debug-feedback`: the
BMP screenshot, the player's note, and a `.game.json` snapshot. This means a
report made during Helgen records MQ101's current/done stages, recent stage
events, controls, camera, menus, and player state without a second manual step.

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
