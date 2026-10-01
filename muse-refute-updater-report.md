# Refutation report: in-game updater (UpdateService + Updates panel)

Scope: read-only review of `Code/client/Services/UpdateService.h`,
`Code/client/Services/Generic/UpdateService.cpp`, its wiring
(`Code/client/World.cpp:189`, `Code/client/World.h:77`,
`Code/client/Services/Generic/OverlayClient.cpp:108-120`,
`Code/tp_process/ProcessHandler.cpp:64-65`, `Code/client/xmake.lua:73-77`)
and the CEF UI (`settings.component.ts/html`, `client.service.ts`,
`typings.d.ts`, mock). No build, no game run, no file edits.
Contract reference: `C:\Tools\skyrim_re\agent\codex-starter-release-prompt.md`.

Verdict summary: claims 3, 4, 5 hold; claim 6 holds except at shutdown;
claim 1 holds in steady state but is refuted at shutdown (hang + spawn race);
claim 2 holds for every vector enforced in repo code, with one UNVERIFIED
residual (pre-extraction zip entry names). Claim 7: game side matches the
starter contract; three coupling risks live outside the game code.

Threading premise for claim 1 is real: `OverlayClient.cpp:68-70` shows
handlers that need the game thread must explicitly `GetRunner().Queue(...)`
("push to main thread"), so `OnProcessMessageReceived` runs off the game
thread, while `OnUpdate` (`UpdateService.cpp:504-522`) runs on the game
thread via `World::Update` (`World.cpp:257-259`).

## Claim 1 — Thread safety — MIXED (holds in life, REFUTED at shutdown)

HOLDS in steady state:

- `m_busy` is the spawn mutex and it works. `CheckNow` (`287-290`) and
  `Download` (`301-308`) both gate on the same `m_busy.exchange(true)`, so at
  most one thread reaches `Join()` + `m_worker` assignment. The worker always
  clears `m_busy` as its last statement (`295`, `312`), therefore `Join()`
  (`202-206`) in these paths only ever joins an already-finished thread and
  never blocks. No two threads can race `Join` + assign while the service
  lives.
- All shared strings are under `m_lock` on every path: writes in `RunCheck`
  (`346-354`), reads/writes in `RunDownload` (`368-374`, progress `392-394`),
  `SetPhase` (`271-276`), `Download`'s phase gate (`301-305`), `StateJson`
  (`278-285`). `m_pushed`/`m_nextCheck` are touched only in `OnUpdate`
  (game thread). `m_stopping` is atomic.

REFUTED — (a) destructor can hang game exit on network/tar waits.
`~UpdateService` (`196-200`) sets `m_stopping = true` then `Join()`s on the
destroying (game) thread. Cancellation is advisory only: `m_stopping` is
polled solely in the `HttpGet` progress callbacks (`319`, `391-395`), while
a stalled `WinHttpReadData` (`123`) sits until the 60 s receive timeout
(`92`: 10/10/20/60 s timeouts), and the tar wait is
`WaitForSingleObject(..., 120000)` + 5 s kill wait (`419-423`) on the worker
that `Join()` blocks on. Failing input: click "Check for updates" (or
auto-check fires) on a black-holed connection, then quit within seconds;
exit stalls tens of seconds (worst case ~60 s network + ~125 s tar, if a
download was unpacking).

REFUTED — (b) shutdown spawn race on `m_worker` (narrow). The destructor's
`Join()` does not take the `m_busy` protocol: a CEF-thread `CheckNow` that
won `exchange(true)` and is between `Join()` (`291`) and the `m_worker`
assignment (`293`), or that wins `exchange` after the destructor's `Join`
returned, spawns a worker lambda capturing a dying `this` (`293-296`,
`310-313`), while the destructor concurrently joins/assigns `m_worker`
without a mutex — `std::thread` concurrent join + move-assign is a data
race, and the worker then touches destroyed `m_lock`/strings/`m_world`.
Requires quitting within milliseconds of clicking Check/Update.

## Claim 2 — Write confinement — HOLDS for coded vectors, one UNVERIFIED residual

HOLDS, with the enforcing lines:

- Version text: `latest` must match
  `^\d{1,6}\.\d{1,6}\.\d{1,6}(-[0-9A-Za-z.-]{1,40})?$` (`339-344`) — no `/`,
  `\`, `..`, drive, or quoting characters, so `UpdatesRoot()/latest`
  (`383-385`) cannot escape `%LOCALAPPDATA%\SkyrimSEMultiplayer\updates\`
  (`42-50`). Empty-root case fails closed (`377-382`).
- Zip URL: must start with the releases/download prefix and contain no `..`,
  scheme forced to https (`80`), redirect policy blocks https-to-http
  (`94-95`), sha must be 64 hex (`377`). Non-ASCII URLs die in
  `WinHttpCrackUrl` (`80-84`) — fails closed.
- Tar invocation is injection-safe: every interpolated component is quoted
  (`409-410`), `latest` cannot contain quotes/spaces per the regex above,
  and `"` cannot appear in a Windows folder path (`UpdatesRoot`).
- Manifest paths: `SafeDataPath` (`174-187`) rejects empty, >260 chars,
  leading `/`/`\`, any `:`, any `..`, any `\`, trailing `/`, `//`, and
  enforces the top-level allowlist (6 roots + 2 root esps, `181-186`). ADS
  (`path:stream`), drive-absolute, and backslash tricks are all rejected.
  Single-dot segments (`a/./b`) normalize inside the payload — harmless.
  Case variants (`SCRIPTS/x`) are rejected (fail closed).
- Archive content: post-extraction walk (`447-469`) rejects reparse points
  (symlinks/junctions, `451-455`, including dangling links whose attributes
  read `INVALID_FILE_ATTRIBUTES`) and any on-disk file not in the manifest
  list (`459-463`); then every listed file is re-checked against
  `SafeDataPath` and its SHA-256 (`477-481`) before anything is staged.
  `payload / fs::u8path(path)` with a validated path stays in the payload.

UNVERIFIED residual (the one gap): zip entry names are never validated
*before* extraction. `tar.exe -xf ... -C payload` (`409-410`) runs first;
the walk (`447-469`) only sees what landed *inside* the payload. If a
`bsdtar` on some target machine honored an absolute entry or a `../` escape
instead of sanitizing it, that write would land outside
`updates\<version>\` and be invisible to every check in this file.
Concrete candidate input: a zip entry named `../../evil.dll` or
`C:/evil.dll`. I could not confirm `tar.exe`'s default sanitization from
repo code (it depends on the OS-shipped libarchive, not on anything here),
so this is UNVERIFIED, not refuted — but it is the top hardening item:
pre-pass `tar -tf`, validate each name with `SafeDataPath`, then extract.

## Claim 3 — Partial/corrupt download never becomes ready — HOLDS

Every gate is fail-closed and ordered correctly:

- Zip hash gate before unpack (`400-404`).
- Tar exit-code gate, including the 120 s timeout kill path which leaves
  `exitCode != 0` (`418-432`).
- Non-empty `files` list required (`437-441`).
- Walk rejects links and unlisted files (`451-463`); the per-file loop
  (`471-483`) requires `SafeDataPath` + exact SHA-256, and a missing file
  makes `Sha256File` return `{}` (`157-171`) which cannot equal the 64-char
  manifest hash — so listed-but-absent also fails.
- `pending.json` is written only after all of the above (`470-494`), via
  tmp-file + rename (`491-494`).
- Crash-ordering is safe: `ready` (`486-488`) is created *before*
  `pending.json`; a crash between them leaves `ready` without `pending.json`,
  and `RunCheck` stages only when *both* exist (`358-359`), so the next
  check re-downloads instead of advertising ready.

Robustness nit (low, shared with claim 7): the `pending.json` stream itself
is unchecked (`491-493`); only the rename error is tested (`495`). On a
full disk a truncated tmp renames cleanly and phase still becomes `ready`
(`500`) — the starter re-hashes everything so it fails safe (parse error,
no apply), but the UI will have promised "ready". Check `file.good()` /
size before rename.

## Claim 4 — Semver precedence — HOLDS on the valid domain

`IsOlder` (`216-269`) is correct semver 2.0 for valid inputs: build metadata
stripped (`220`), 3-part numeric core (`224-230`), release > prerelease
(`255-256`), numeric identifiers compared by length-then-lexically (exact
for digit strings, `262-263`), numeric < alphanumeric (`264-265`), ASCII
order within alphanumeric (`266`), shorter prerelease < longer on prefix
match (`268`). Dev build `0.0.0-dev+<commit>` (`208-214`): core all zeros
with a non-empty prerelease, so it is older than every `X.Y.Z` release.
No valid-domain inputs were found that compare wrongly.

Caveats (info; the second is a claim-7 coupling):

- `CurrentVersion` trusts `BUILD_RELEASE_VERSION` verbatim (`211-212`).
  Concrete failing input: env value `v1.2.3` (v not stripped) parses as
  core `{0,1,3}` (`strtoull("v1...") == 0`, then shifted parts), so the
  build looks older than everything down to `0.1.3` — perpetual "available".
  Defense lives outside this file (release workflow must strip `v`; the
  starter prompt's Task 2 does say "tag without v").
- Invalid inputs are merely lenient, never escapes: `"1"` == `"1.0.0"`,
  a 4th part is ignored, `strtoull` clamps huge parts, `"01"` == `"1"`.
  The validated side (`latest`, regex `339`) can never supply these.
- Pedantry, correctly handled: `IsOlder("0.0.0-dev+x", "0.0.0-aaa")` is
  false (`"dev" > "aaa"` lexically) — right per semver, so "older than any
  release" holds for releases but not for every 0.0.0 prerelease; no real
  manifest will contain those.

## Claim 5 — UI rendering/status/title-screen state — HOLDS

- Never HTML: patch notes render via interpolation `{{ line.text }}` only
  (`settings.component.html:234`), which Angular escapes; the getter
  (`settings.component.ts:379-397`) additionally strips headings, bullets,
  `**`, backticks, and `[text](url)` links into plain text (200-line cap).
  No `[innerHTML]` anywhere on this path.
- Status lines cover all 8 native phases (`353-377`: idle/checking/current/
  available/downloading/verifying/ready/offline/error plus `default: ''`).
  Download percent guards `size == 0` (`365`) and clamps to 100 (`365`).
- Title-screen state: `ngOnInit` sends one `checkUpdates()` when no state
  exists yet (`150-151`), and `OverlayClient` answers synchronously in the
  handler (`115-119`, "Answered right away too: the title screen may not run
  the update tick") without depending on the runner queue — unlike e.g.
  `acceptPartyInvite` (`68-70`). Push updates also flow via `updateState`
  (`client.service.ts:219`, typings `219/371`, mock present).
- Nits (info): `idle` displays "Checking for updates..." though nothing is
  in flight; unknown future phases display `''`; `restoreSection`'s
  allowlist (`222`) omits `'updates'`, so reopening settings after visiting
  the Updates tab falls back to Display.

## Claim 6 — Never blocks on network; offline is a status — HOLDS except shutdown

- Steady state holds: `CheckNow`/`Download` only flip `m_busy` and spawn
  (`287-314`); all WinHTTP runs on the worker (`319`, `391`); the game
  thread only formats JSON and calls async `ExecuteAsync` (`513-521`).
  Offline maps to phase `offline` + error string (`325`) and the UI line
  "Could not reach GitHub..." (`370-371`), with no retry storm (6 h timer
  `507-510`, manual button otherwise).
- Shutdown exception: see claim 1(a) — quitting mid-check/download joins
  the worker on the game thread through WinHTTP (up to ~60 s) and tar
  (~125 s) waits. The "never blocks" claim is refuted for the exit path.

## Claim 7 — Release-to-player path vs the starter contract — game side HOLDS, 3 couplings

Game-side artifacts match the starter prompt exactly: `pending.json`
`{version, payload, files[{path, sha256-UPPER}]}` (`470`, `476`, `482`),
absolute forward-slash payload path (`470`), verified files at
`payload/<path>`, empty `ready` file (`486-488`), zip deleted after verify
(`485`), version/paths re-validated to the starter's regex/allowlist shape.
`staged` detection (`358-359`: pending.json + `<latest>/ready`) mirrors the
starter's trigger. `Download` gating on `available` (`301-305`) plus the
Check button gives a clean error-recovery loop.

Couplings that can still break the end-to-end path (all outside game code):

1. `BUILD_RELEASE_VERSION` must actually be set by `release.yml` from the
   tag (consumed in `Code/base|common|components/xmake.lua:4-5`; nothing in
   the game validates it — claim 4's `v1.2.3` case). Unset/empty means every
   install permanently reports "available".
2. Auto-check "once at start" assumes `World::Update` ticks wherever the
   player idles; `OverlayClient.cpp:115` openly doubts the title screen runs
   the tick. If it does not, background checks only happen in-game (manual
   check still works). UNVERIFIED without running the game.
3. Manifest authoring (`make-manifest.py`, notes embed, zip layout rooted at
   Data, URL under `releases/download/`, non-empty `files`) must satisfy the
   game's strict gates (`332-344`, `377-382`, `437-441`) — any slip becomes
   an `error` dead end, and a manifest that *regresses* `latest` below a
   staged pending version leaves a stale pending.json the starter will
   still apply while the UI says "current" (low).

## Ranked defects (all minor; no ship-blocker found)

1. Low (robustness/UX): exit stall — advisory-only `m_stopping` + joining
   destructor across WinHTTP 60 s and tar 125 s waits (`92`, `196-200`,
   `319`, `391-395`, `419-423`). Claims 1, 6.
2. Low (hardening, UNVERIFIED): no pre-extraction validation of zip entry
   names; outside-payload writes would bypass the `447-469` walk
   (`406-432`). Claims 2, 7. Suggested: `tar -tf` pre-pass vs
   `SafeDataPath`.
3. Low (shutdown race): CEF thread can spawn a `this`-capturing worker
   across destructor `Join` (`287-314` vs `196-200`). Claim 1.
4. Low: unchecked `pending.json` tmp stream before rename/`ready`
   (`486-499`). Claims 3, 7.
5. Info: unvalidated `BUILD_RELEASE_VERSION` (`208-214`); `v`-prefix input
   miscompares (`216-230`). Claims 4, 7.
6. Info: UI nits — `idle` status text, empty `default` phase text, Updates
   tab not restored (`settings.component.ts:353-377`, `:222`). Claim 5.
