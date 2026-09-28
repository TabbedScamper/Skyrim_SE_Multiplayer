# coop-replay JSONL, version 1

UTF-8, LF, one object per line, finite JSON numbers only. Integers are exact
unsigned values in the Python/C++ readers; consumers using JavaScript must use
an integer-preserving parser for 64-bit ticks, graph identifiers and slot masks.
Unknown additive payload fields must be preserved. A changed meaning or unit
requires a new version. An unsupported version is an error.

Every row has these fields:

| Field | Meaning |
|---|---|
| `schema`, `version` | `coop-replay`, `1` |
| `capture`, `run`, `epoch`, `session` | Capture folder; harness run and authority epoch if captured; per-file lobby session ordinal. Null run is unknown, never a match wildcard. |
| `peer` | Host, Follower, or Unknown for these legacy captures. Future recordings use unique peer IDs plus an authority ID; peer count is not limited to two. |
| `tick` | Captured shared network tick in ms, or null. Never synthesized from wall time. |
| `wall_ms` | Process-local monotonic time, or null. Never directly comparable between PCs. |
| `phase` | send, receive, applied, or observation. A frame observed on Host is not labelled send. |
| `kind`, `payload` | Typed channel below. Legacy payloads retain the complete original object. |
| `source` | Relative capture path, one-based line (1 for JSON), SHA-256 of the original file. |

The extractor emits these legacy channels:

| Channel | Payload and coverage |
|---|---|
| body_frame | Harness frame: `tick`, `frame`, `cell`, `refs[].id`, `refs[].p`, `exists`, `loaded`. `p` is a reference position in game units, **not** a Havok body transform. Velocities and body rotations are absent. |
| actor_pose | `id`, `pelvisLeftRight`, `variable01`, state flags. These three skeleton nodes are not a complete ragdoll body set. |
| worn | `entity`, `server`, `ownership_epoch`, `sequence`, owner/local lists of `{mod,base,slots}`, log time, log phase. Null local means not captured; [] is measured empty. Extra data/stock counts are explicitly not captured. |
| quest_state, quest_event | Captured quest IDs/stages or precondition snapshots. No invented intermediate stages. |
| scene | Scene ID, playing flag, raw phase; `4294967295` remains the native unknown sentinel. |
| world_node, reference | Observed reference/node positions and collision flags, not an inferred animation event. |
| actor_animation | Snapshot animation event, graph/action/idle IDs and native graph state when present. |
| camera | Raw camera state, camera authority and player rotation in radians kept separately. `pitch_rad` and `intent_pitch_rad` remain null when not captured. Player pitch is not silently treated as rendered camera pitch. |
| trigger, movement_driver, camera_trace | Original bounded event/driver/camera trace fields. |
| armor, client_diagnostic, snapshot, harness_event | Lossless supporting evidence. Armor pointers are not worn item IDs. `nonAtomic`, `truncated`, missing values and failure events are retained. |

Future producers can use the same envelope with these canonical channels. They
are an adapter contract, not a claim that legacy logs contain this information:

* `body`: entity identity `(server, ownership_epoch, topology_revision, body_key)`,
  family cart/horse/debris/ragdoll, transform `{position:[x,y,z], rotation:[x,y,z,w]}`,
  linear velocity `[x,y,z]`, angular velocity `[x,y,z]`, settled flag, cell/world,
  host sample tick, presentation tick, apply stage (pre-step/post-step/render).
  Position/linear velocity use game units and game units/s; angular velocity rad/s.
  Body-local offsets require a separate origin and may not masquerade as world positions.
* `actor`: stable entity/epoch, position, Euler rotation radians, velocity,
  movement direction, named or descriptor-indexed animation variables with types.
* `world_animation`: stable reference identity, event name, sequence, graph/content
  identity, clip fraction/end state and collision state. Quest stage is not completion.
* `camera`: `pitch_rad`, `intent_pitch_rad`, explicitly configured tolerance,
  camera state ID, control owner, input/script/spectate context.
* `worn`: full item-instance extras and authoritative stock in addition to the
  legacy form/slot projection; include the completeness flag and epoch fencing.

Unknown evidence is `null` or absent, never a zero measurement or a pass. Missing
run/epoch/identity/body topology prevents paired acceptance. Offline comparison
of reference frames uses bracketing network ticks only, within 50 ms, consecutive
loaded host frames in one cell and the same run. It reports an observation gap;
it cannot establish the intended presentation delay. Reloads and gaps break step
continuity. Worn pairing requires exact capture/entity/server/epoch/sequence/items
and a 10-second sanity window for the two log wall clocks.

## RPL1 test transport

The small committed regression subset is also compiled to binary so TPTests
needs no JSON dependency. All integers are little endian, doubles IEEE-754 f64:

* Magic `RPL1`, u32 case count (1..1,000,000).
* Each case: u32 kind, u32 provenance byte length (max 65,536), UTF-8 JSON provenance.
* Kind 1 cart step: previous xyz and current xyz, six f64.
* Kind 2 worn: owner then local; each is u32 count (max 35) followed by
  u32 mod, u32 base, u64 slot bits for each item.
* Kind 3 cart gap / kind 4 horse gap: host first xyz, host second xyz, observed
  follower xyz (nine f64), then u64 first tick, second tick, follower tick.
* Kind 5 camera: u8 presence mask (bits 0/1/2 for pitch/intent/tolerance),
  three f64 radians. Absent fields have zero wire padding, decoded as missing.
* Kind 6 bone: u8 settled, six f64 target/observed positions in Havok units,
  eight f64 target/observed xyzw quaternions. Provenance must identify a matched
  body key/topology, same presentation tick and phase. The runner applies the
  70-unit conversion, quaternion angle metric and settled/falling targets.
* Kind 7 debris: u8 settled, six f64 target/observed positions in game units;
  the runner applies the <5 settled / <30 moving target.

Kinds 6/7 are supported for future full captures; the historical subset has
none. This is reported as MISSING, not accepted from zero residual log lines.

The reader rejects truncation, unknown magic/kinds, invalid count/slots/timeline,
and trailing bytes. The JSONL subset remains authoritative. Regenerate binary
with `extract.write_binary`; byte identity is tested. Larger new channels require
an explicit reader extension; unsupported evidence cannot pass silently.
