"""Offline paired JSONL analysis. Never contacts or samples a running game."""
import collections
import json
import math
import pathlib
import sys


def records(path):
    """Strict acceptance reader: never repair or skip incomplete evidence."""
    def reject_constant(value):
        raise ValueError(f'{path.name}: non-JSON numeric constant {value}')

    with path.open('rb') as stream:
        for number, raw in enumerate(stream, 1):
            if not raw.endswith(b'\n'):
                raise ValueError(f'{path.name}:{number}: truncated JSONL record')
            row = json.loads(raw.decode('utf-8-sig'), parse_constant=reject_constant)
            if not isinstance(row, dict):
                raise ValueError(f'{path.name}:{number}: expected object')
            yield row


def validate_capture(root, stamp, accepted):
    """Validate a proposed pass against the collected files, independently of polling."""
    scenario = json.loads((root / 'scenario.json').read_text(encoding='utf-8-sig'))
    health = json.loads((root / 'session-health.json').read_text(encoding='utf-8-sig'))
    artifacts = json.loads((root / 'artifact-identity.json').read_text(encoding='utf-8-sig'))
    final_sequence = len(scenario['steps'])
    if not final_sequence or scenario['steps'][-1].get('op') != 'finish':
        raise ValueError('scenario has no terminal finish')
    paired = []
    leader_ids = {}
    for pc in ('Host', 'Follower'):
        folder = root / pc
        deployed = artifacts['clients'][pc]
        for name in ('SkyrimTogether.exe', 'STServer.dll', 'SkyrimTogetherServer.exe', 'TPProcess.exe'):
            digest = artifacts['expected'][name]
            if len(digest) != 64 or any(c not in '0123456789abcdefABCDEF' for c in digest) or deployed['hashes'][name] != digest:
                raise ValueError(f'{pc}: artifact bundle mismatch: {name}')
        if deployed['gamePid'] != health['clients'][pc]['logs']['pid']:
            raise ValueError(f'{pc}: artifact identity PID differs from session health')
        if pc == 'Host' and deployed['serverPid'] != health['serverPid']:
            raise ValueError('artifact server PID differs from session health')
        logs = list(folder.glob('tp_client*.log'))
        if not logs or any(p.stat().st_size == 0 for p in logs if p.name == 'tp_client.log'):
            raise ValueError(f'{pc}: missing/empty client log')
        log_text = '\n'.join(p.read_text(encoding='utf-8-sig', errors='replace') for p in logs)
        evidence = health['clients'][pc]['logs']
        for line in evidence['lines']:
            if line not in log_text:
                raise ValueError(f'{pc}: collected logs omit preflight evidence')
        if not evidence['scan'] or not evidence['joined'] or not evidence['lines']:
            raise ValueError(f'{pc}: incomplete session health')
        last_status = None
        for status in records(folder / f'harness-{stamp}.status.jsonl'):
            if status.get('stamp') != stamp or status.get('dropped') != 0:
                raise ValueError(f'{pc}: wrong stamp or dropped captures')
            last_status = status
        expected = accepted[pc]
        if not last_status or any(last_status.get(k) != expected.get(k) for k in
                                  ('state', 'run', 'epoch', 'stamp', 'sequence', 'metric', 'dropped')):
            raise ValueError(f'{pc}: collected terminal status disagrees with accepted status')
        if (last_status['state'] != 'passed' or last_status['active'] or last_status.get('error') or
                last_status['sequence'] != final_sequence or last_status['metric'] != scenario['metric'] or
                int(last_status['run']) <= 0 or int(last_status['epoch']) <= 0):
            raise ValueError(f'{pc}: invalid terminal pass')
        identity = (last_status['run'], last_status['epoch'])
        paired.append(identity)
        sequence = 0
        phase = 0
        terminal = None
        frames = 0
        party_proof = False
        # Each step must have its own local preparation, release, completion,
        # and server barrier in order; valid JSON with a missing tail is not enough.
        phases = {'precondition_ack': 1, 'precondition_release': 2, 'step_done': 3, 'barrier': 4}
        for row in records(folder / f'harness-{stamp}.jsonl'):
            kind = row.get('kind')
            if terminal is not None:
                raise ValueError(f'{pc}: records after actions_complete')
            if 'run' in row and row['run'] != identity[0]:
                raise ValueError(f'{pc}: mixed run IDs')
            if 'epoch' in row and row['epoch'] != identity[1]:
                raise ValueError(f'{pc}: mixed epochs')
            if kind == 'failed':
                raise ValueError(f'{pc}: failed record in proposed pass')
            if kind == 'tower_tcl_bypass' and scenario['metric'] == 'collision-on':
                raise ValueError(f'{pc}: TCL in collision pass')
            if kind == 'step':
                if row['sequence'] != sequence + 1 or (sequence and phase != 4):
                    raise ValueError(f'{pc}: missing or unordered step/barrier')
                sequence += 1
                if sequence > final_sequence or row.get('op') != scenario['steps'][sequence - 1]['op']:
                    raise ValueError(f'{pc}: scenario step mismatch')
                for key, value in scenario['steps'][sequence - 1].items():
                    if key == 'preconditions':
                        if any(row.get(key, {}).get(k) != v for k, v in value.items()):
                            raise ValueError(f'{pc}: scenario precondition mismatch')
                    elif row.get(key) != value:
                        raise ValueError(f'{pc}: scenario parameter mismatch: {key}')
                if scenario['metric'] == 'collision-on' and (row.get('cmd') == 'tcl' or row.get('fallback')):
                    raise ValueError(f'{pc}: TCL step in collision pass')
                phase = 0
                party_proof = False
            elif kind in phases:
                if row['sequence'] != sequence or phases[kind] != phase + 1:
                    raise ValueError(f'{pc}: incomplete local step evidence')
                if kind.startswith('precondition_') and not row.get('ready'):
                    raise ValueError(f'{pc}: precondition was not ready')
                if kind == 'step_done' and scenario['steps'][sequence - 1].get('party_trigger') and not party_proof:
                    raise ValueError(f'{pc}: missing party trigger evidence')
                phase += 1
            elif kind in ('local_trigger_enter', 'party_trigger_proximity_arrived'):
                step = scenario['steps'][sequence - 1]
                if step.get('party_trigger'):
                    if phase != 2 or row.get('sequence') != sequence or row.get('trigger') != int(step['until_trigger'], 16):
                        raise ValueError(f'{pc}: misplaced party trigger evidence')
                    if pc == 'Host':
                        if kind != 'local_trigger_enter' or row.get('entrant') != 0x14 or row.get('generation', 0) <= 0:
                            raise ValueError(f'{pc}: leader requires original local trigger entry')
                        if not isinstance(row.get('localId'), int) or row.get('leaderId') != row['localId']:
                            raise ValueError(f'{pc}: invalid leader identity')
                        leader_ids[sequence] = row['localId']
                    else:
                        if kind != 'party_trigger_proximity_arrived':
                            raise ValueError(f'{pc}: follower requires completed proximity walk')
                        if (row.get('leaderId') != leader_ids.get(sequence) or not isinstance(row.get('localId'), int) or
                                row['localId'] == row['leaderId']):
                            raise ValueError(f'{pc}: invalid leader identity')
                        wait = step['party_wait']
                        actual = [row['actual' + axis.upper()] for axis in 'xyz']
                        trigger = [row['trigger' + axis.upper()] for axis in 'xyz']
                        target = [wait[axis] for axis in 'xyz']
                        values = actual + trigger + target + [row['targetDistance'], row['triggerDistance']]
                        if not all(isinstance(v, (float, int)) and math.isfinite(v) for v in values):
                            raise ValueError(f'{pc}: nonfinite party trigger coordinates')
                        target_distance, trigger_distance = math.dist(actual, target), math.dist(actual, trigger)
                        if (any(row.get(k) != v for k, v in wait.items()) or
                                target_distance > wait.get('radius', 8) or
                                abs(target_distance - row['targetDistance']) > 1 or
                                abs(trigger_distance - row['triggerDistance']) > 1):
                            raise ValueError(f'{pc}: invalid party trigger proximity')
                        if row.get('proximityTo') == 'leader':
                            if not isinstance(row.get('leaderForm'), int) or row['leaderForm'] in (0, 0x14):
                                raise ValueError(f'{pc}: missing remote leader identity')
                            leader = [row['leader' + axis.upper()] for axis in 'xyz']
                            if (not all(isinstance(v, (float, int)) and math.isfinite(v) for v in leader + [row['leaderDistance']]) or
                                    math.dist(actual, leader) > 600 or
                                    abs(math.dist(actual, leader) - row['leaderDistance']) > 1):
                                raise ValueError(f'{pc}: invalid party leader proximity')
                        elif row.get('proximityTo') != 'trigger' or trigger_distance > 400:
                            raise ValueError(f'{pc}: invalid party trigger proximity basis')
                    party_proof = True
            elif kind == 'frame':
                frames += 1
                if row.get('frame') != frames:
                    raise ValueError(f'{pc}: missing/unordered frame record')
            elif kind == 'actions_complete':
                if sequence != final_sequence or row['sequence'] != sequence or phase != 4:
                    raise ValueError(f'{pc}: premature terminal record')
                terminal = row
        if terminal is None or frames == 0:
            raise ValueError(f'{pc}: missing terminal record or frame stream')
    server = root / 'Host' / 'STServerOut.log'
    if not server.is_file() or server.stat().st_size == 0:
        raise ValueError('missing/empty server log')
    if 'Harness server buildTag=' + health['serverBuildTag'] not in server.read_text(encoding='utf-8-sig', errors='replace'):
        raise ValueError('collected server log omits build evidence')
    if paired[0] != paired[1]:
        raise ValueError('paired terminal run/epoch mismatch')
    return {'valid': True, 'stamp': stamp, 'run': paired[0][0], 'epoch': paired[0][1], 'steps': final_sequence}


def quantiles(values):
    values = sorted(values)
    if not values:
        return {}
    return {"n": len(values), "p50": values[(len(values)-1)//2],
            "p95": values[int((len(values)-1)*.95)], "max": values[-1]}


def analyze(folder):
    result = {"frames": 0, "badLines": 0, "steps": [], "events": [], "scenes": [], "drivers": [], "references": [], "armor": {}, "cells": [],
              "preconditions": [], "collisionByStep": {}, "collisionExecutionByStep": {}, "probeFinal": {}}
    timings = collections.defaultdict(list)
    armor = collections.defaultdict(collections.Counter)
    probes = collections.defaultdict(list)
    poses = collections.defaultdict(lambda: collections.defaultdict(list))
    cart_stats = collections.defaultdict(lambda: {"samples": 0, "distance": 0.0, "largestDelta": 0.0})
    cart_previous = {}
    sequence = 0
    operations = {}
    executing = False
    cells = set()
    for path in folder.glob("harness-*.jsonl"):
        with path.open(encoding="utf-8-sig") as stream:
            for line in stream:
                try:
                    row = json.loads(line)
                except ValueError:
                    result["badLines"] += 1
                    continue
                kind = row.get("kind")
                if kind == "step":
                    sequence = row.get("sequence", 0)
                    operations[sequence] = row.get("op")
                    executing = False
                elif kind == "precondition_release":
                    executing = True
                if kind == "frame":
                    result["frames"] += 1
                    cells.add(row.get("cell"))
                    result["lastCell"] = row.get("cell")
                    if result["frames"] == 1:
                        result["firstFrameGapMs"] = row.get("gapMs", 0)
                    for field in ("gapMs", "previousTickCaptureUs", "vmAppUs", "vmNativeUs"):
                        # A run's first record has no preceding captured frame.
                        # Preserve its raw gap separately; older continuation
                        # builds carried the preceding run's clock into it.
                        if field == "gapMs" and result["frames"] == 1:
                            continue
                        # Legacy captureUs was also the preceding tick's cost.
                        timings[field].append(row.get(field, row.get('captureUs', 0) if field == 'previousTickCaptureUs' else 0))
                    for ref in row.get("refs", []):
                        if not ref.get("p") or not ref.get("loaded"):
                            continue
                        key = f'{ref["id"]:08X}'
                        stat = cart_stats[key]
                        stat["samples"] += 1
                        if key in cart_previous:
                            delta = math.dist(ref["p"], cart_previous[key])
                            stat["distance"] += delta
                            stat["largestDelta"] = max(delta, stat["largestDelta"])
                        cart_previous[key] = ref["p"]
                elif kind in ("step", "capture", "failed", "passed", "actions_complete", "tower_path_failed", "tower_tcl_bypass"):
                    result["steps"].append(row)
                elif kind == "events":
                    compact = {key: row.get(key) for key in ("MQ101", "MQ101DragonAttack", "doorBits")}
                    if not result["events"] or compact != result["events"][-1]["state"]:
                        result["events"].append({"wallMs": row.get("wallMs"), "state": compact})
                elif kind == "scene":
                    result["scenes"].append(row)
                elif kind in ("precondition_wait", "precondition_ack", "precondition_release"):
                    result["preconditions"].append(row)
                elif kind == "collision":
                    seq = row.get("sequence", sequence)
                    groups = ["collisionByStep"] + (["collisionExecutionByStep"] if executing else [])
                    for group in groups:
                        stat = result[group].setdefault(str(seq), {
                            "op": operations.get(seq), "samples": 0, "globalOff": 0,
                            "referenceOff": 0, "controllerOff": 0, "controllerUnknown": 0})
                        stat["samples"] += 1
                        stat["globalOff"] += not row.get("collisionOn", False)
                        stat["referenceOff"] += bool(row.get("playerNoCollision"))
                        stat["controllerOff"] += bool(row.get("controllerNoCollision"))
                        stat["controllerUnknown"] += not row.get("filterReadable", False)
                elif kind == "reference":
                    result["references"].append(row)
                elif kind in ("driver", "path_stall_recovery", "trigger_walk_submission", "local_trigger_enter", "party_trigger_proximity_arrived"):
                    result["drivers"].append(row)
                elif kind == "armor":
                    counts = armor[f'{row["id"]:08X}']
                    counts["samples"] += 1
                    for key in ("readable", "skinned", "parentReadable", "attachedWithin16"):
                        counts[key] += bool(row.get(key))
                    counts["nullClone"] += not bool(row.get("clone"))
                elif kind == "probe":
                    key = f'{row["id"]:08X}/{row["node"]}'
                    value = {key: row.get(key) for key in ("p", "flags", "collision", "missing", "truncated")}
                    result["probeFinal"][key] = {"wallMs": row.get("wallMs"), "value": value}
                    if not probes[key] or probes[key][-1]["value"] != value:
                        if len(probes[key]) < 100:
                            probes[key].append({"wallMs": row.get("wallMs"), "value": value})
                elif kind == "pose":
                    bones = row.get("pelvisLeftRight", [])
                    if len(bones) == 3 and all(bones) and not row.get("truncated"):
                        pelvis, left, right = bones
                        key = f'{row["id"]:08X}/step{sequence}'
                        poses[key]["handSeparation"].append(math.dist(left, right))
                        poses[key]["handHeightAbovePelvis"].append((left[2]+right[2])/2-pelvis[2])
    result["cells"] = sorted(c for c in cells if c is not None)
    result["timings"] = {key: quantiles(values) for key, values in timings.items()}
    result["armor"] = dict(armor)
    result["probes"] = dict(probes)
    result["poseMeasurements"] = {key: {name: quantiles(values) for name, values in data.items()} for key, data in poses.items()}
    result["cartMotion"] = dict(cart_stats)
    return result


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--validate':
        root = pathlib.Path(sys.argv[2])
        accepted = json.loads((root / 'accepted-status.json').read_text(encoding='utf-8-sig'))
        try:
            validation = validate_capture(root, sys.argv[3], accepted)
        except (ValueError, KeyError, OSError, TypeError) as error:
            validation = {'valid': False, 'error': str(error)}
        (root / 'evidence-validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
        print(json.dumps(validation))
        sys.exit(0 if validation['valid'] else 1)

    root = pathlib.Path(sys.argv[1])
    report = {pc: analyze(root / pc) for pc in ("Host", "Follower")}
    (root / "analysis.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    lines = ["", "Offline capture measurements", "", "| PC | Frames | Frame gap p50/p95/max ms | Capture p50/p95/max us | Last cell |", "|---|---:|---|---|---|"]
    notes = []
    for pc, data in report.items():
        def stat(key):
            item = data["timings"].get(key, {})
            return "/".join(str(item.get(k, "-")) for k in ("p50", "p95", "max"))
        lines.append(f'| {pc} | {data["frames"]} | {stat("gapMs")} | {stat("previousTickCaptureUs")} | {data.get("lastCell", 0):08X} |')
        failures = [s.get("reason", "") for s in data["steps"] if s.get("kind") == "failed"]
        if failures:
            notes.append(f'\n{pc} failure: {failures[0]}')
    lines.extend(notes)
    lines.extend(["", "Capture duration belongs to the PRECEDING tick (including legacy captureUs). The cost of a hitch frame appears in the following frame record; neither establishes causation.", "Attachment samples are non-atomic. A disconnected skinned clone alone does not prove a visible floating head. Pose, wall cause, and door-crash conclusions require the corresponding observations; missing records are not passes."])
    summary = root / "summary.md"
    existing = summary.read_text(encoding="utf-8-sig") if summary.exists() else ""
    existing = existing.split("\nOffline capture measurements")[0].rstrip()
    summary.write_text(existing + "\n" + "\n".join(lines) + "\n", encoding="utf-8")
    print(root / "analysis.json")


if __name__ == '__main__':
    main()
