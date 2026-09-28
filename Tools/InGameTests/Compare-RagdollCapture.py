"""Offline comparison of bounded host/follower ragdoll JSONL captures.

Nearest unused rendered samples must differ by <=50 ms on host tick versus
follower presentationMs. No interpolation hides a missing observation. Render
reads are non-atomic: a passing result describes captured samples, not continuous
fall, equal wall-clock instants, or a complete gameplay/performance validation.
"""
import argparse
import json
import math
from pathlib import Path


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def vector(value, size):
    return isinstance(value, list) and len(value) == size and all(number(x) for x in value)


def quaternion(m):
    trace = m[0] + m[4] + m[8]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = [(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, s / 4]
    else:
        i = max(range(3), key=lambda x: m[4 * x])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(max(0, 1 + m[4 * i] - m[4 * j] - m[4 * k])) * 2
        if s == 0:
            raise ValueError("invalid rotation matrix")
        q = [0.0] * 4
        q[i], q[j], q[k], q[3] = s / 4, (m[3 * i + j] + m[3 * j + i]) / s, (m[3 * i + k] + m[3 * k + i]) / s, (m[3 * k + j] - m[3 * j + k]) / s
    norm = math.sqrt(sum(x * x for x in q))
    return [x / norm for x in q]


def angle(a, b):
    na, nb = math.sqrt(sum(x*x for x in a)), math.sqrt(sum(x*x for x in b))
    if na < 0.99 or nb < 0.99 or na > 1.01 or nb > 1.01:
        raise ValueError("non-unit quaternion")
    return math.degrees(2 * math.acos(min(1, abs(sum(x*y for x, y in zip(a, b)) / (na*nb)))))


def valid_matrix(m):
    if not vector(m, 9):
        return False
    rows = [m[x:x+3] for x in (0, 3, 6)]
    if any(abs(sum(x*x for x in row)-1) > 0.01 for row in rows):
        return False
    if any(abs(sum(x*y for x, y in zip(rows[i], rows[j]))) > 0.01 for i, j in ((0,1), (0,2), (1,2))):
        return False
    determinant = m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6])
    return abs(determinant-1) < 0.02


def read_capture(path):
    path = Path(path)
    files = sorted(path.rglob("*.jsonl")) if path.is_dir() else [path]
    records, problems = [], []
    if not files:
        problems.append(f"No JSONL capture files: {path}")
    for file in files:
        try:
            lines = file.read_text(encoding="utf-8-sig").splitlines()
        except OSError as error:
            problems.append(str(error))
            continue
        for line, text in enumerate(lines, 1):
            if not text.strip():
                continue
            try:
                record = json.loads(text)
                if not isinstance(record, dict):
                    raise ValueError("record must be an object")
                record = dict(record, _source=f"{file}:{line}")
                records.append(record)
            except (ValueError, TypeError) as error:
                problems.append(f"Malformed JSONL {file}:{line}: {error}")
    return records, problems


def compare(host, follower, input_problems=()):
    problems = list(input_problems)
    body_rows, render_rows = [], []
    renders = {"host": [], "follower": []}
    errors = []
    non_atomic = 0
    # This invocation evaluates host-owned corpses. Both PCs may additionally
    # capture copies of corpses owned by the other PC; those opposite-authority
    # records cannot substitute for host-owned evidence or invalidate it.
    host_owned = {r.get("serverId") for r in host if r.get("kind") == "ragdoll_render"
                  and r.get("remote") is False and type(r.get("serverId")) is int}
    for side, records in (("host", host), ("follower", follower)):
        for n, r in enumerate(records):
            source = r.get("_source", f"{side}[{n}]")
            kind = r.get("kind")
            if kind == "ragdoll_capture_gap":
                problems.append(f"Capture gap: {source}")
                continue
            if kind not in ("ragdoll_render", "ragdoll_error"):
                continue
            opposite_role = (kind == "ragdoll_render" and isinstance(r.get("remote"), bool)
                             and r["remote"] != (side == "follower")) or (kind == "ragdoll_error" and side == "host")
            if opposite_role and r.get("serverId") not in host_owned:
                continue
            try:
                if not isinstance(r.get("serverId"), int) or r["serverId"] <= 0:
                    raise ValueError("missing stable serverId")
                if not number(r.get("wallMs")) or not number(r.get("tick")) or not number(r.get("presentationMs")):
                    raise ValueError("missing/nonfinite sample times")
                if r.get("dropped", 0) != 0 or r.get("truncated", False):
                    raise ValueError("dropped/truncated observation")
                if kind == "ragdoll_render":
                    if not isinstance(r.get("stateFlags"), int):
                        raise ValueError("missing actor stateFlags")
                    # CommonLib ACTOR_LIFE_STATE: 1 dying, 2 dead. Exclude alive,
                    # downed, reanimated and unrelated knockdown render snapshots.
                    if ((r["stateFlags"] >> 21) & 15) not in (1, 2):
                        continue
                    if r.get("remote") is not (side == "follower"):
                        raise ValueError("capture role does not match owner/follower")
                    if not isinstance(r.get("truncated"), bool) or not isinstance(r.get("nonAtomic"), bool):
                        raise ValueError("missing completeness/atomicity flags")
                    bones = r.get("bones")
                    if type(r.get("boneCount")) is not int or not isinstance(bones, list) or not 0 < len(bones) == r["boneCount"] <= 128:
                        raise ValueError("incomplete bone count")
                    if {b.get("bone") for b in bones} != set(range(len(bones))):
                        raise ValueError("missing/duplicate bone indices")
                    for b in bones:
                        if b.get("nameComplete") is not True or not isinstance(b.get("nameHash"), int):
                            raise ValueError("missing/partial bone name")
                        if not vector(b.get("p"), 3) or not valid_matrix(b.get("r")) or not number(b.get("scale")) or b["scale"] <= 0:
                            raise ValueError("invalid rendered transform")
                    non_atomic += r["nonAtomic"]
                    renders[side].append(r)
                else:
                    if side != "follower":
                        raise ValueError("body error is not from follower")
                    if r.get("phase") != "post-solver-before-correction":
                        raise ValueError("incorrect body measurement phase")
                    for key in ("ownerSettled", "followerResting", "exactAfterMeasurement"):
                        if not isinstance(r.get(key), bool):
                            raise ValueError(f"missing {key}")
                    if not isinstance(r.get("limb"), int) or r["limb"] < 0 or not number(r.get("sampleAgeMs")) or r["sampleAgeMs"] < 0:
                        raise ValueError("invalid limb/target sample age")
                    if type(r.get("dropped")) is not int or r.get("truncated") is not False:
                        raise ValueError("missing dropped/completeness fields")
                    bodies = r.get("bodies")
                    if type(r.get("bodyCount")) is not int or not isinstance(bodies, list) or not 0 < len(bodies) == r["bodyCount"] <= 64:
                        raise ValueError("incomplete body count")
                    if {b.get("body") for b in bodies} != set(range(len(bodies))):
                        raise ValueError("missing/duplicate body indices")
                    for b in bodies:
                        if not all(vector(b.get(k), size) for k, size in (("p",3),("targetP",3),("q",4),("targetQ",4))):
                            raise ValueError("invalid body transform")
                        p, a = math.dist(b["p"], b["targetP"]), angle(b["q"], b["targetQ"])
                        if not number(b.get("positionError")) or not number(b.get("rotationErrorDeg")) or b["positionError"] < 0 or b["rotationErrorDeg"] < 0 or abs(b["positionError"]-p) > 0.1 or abs(b["rotationErrorDeg"]-a) > 0.1:
                            raise ValueError("body error disagrees with captured transforms")
                        if b.get("ownerMotion") not in (1,2,3,4,5,6) or b.get("motion") not in (1,2,3,4,5,6):
                            raise ValueError("invalid motion type")
                        if b["ownerMotion"] in (1,2,3,6) and b["motion"] not in (1,2,3,6):
                            raise ValueError("dynamic owner has nondynamic follower body")
                        if b["motion"] == 4 and b["ownerMotion"] != 4:
                            raise ValueError("follower-only keyframing")
                    if not r["ownerSettled"] and r["sampleAgeMs"] > 100:
                        raise ValueError("moving owner sample older than 100 ms")
                    if r["exactAfterMeasurement"] and not (r["ownerSettled"] and r["followerResting"]):
                        raise ValueError("exact placement without both resting")
                    errors.append(r)
            except (ValueError, TypeError, KeyError, AttributeError) as error:
                problems.append(f"{source}: {error}")

    coverage = {}
    body_counts = {}
    for r in errors:
        key = (r["serverId"], r["limb"])
        if body_counts.setdefault(key, r["bodyCount"]) != r["bodyCount"]:
            problems.append(f"Body set changed without capture generation: {key}")
        coverage.setdefault(key, set())
        phase = "settled" if r["ownerSettled"] and r["followerResting"] else "moving"
        maximum_p = max(b["positionError"] for b in r["bodies"])
        maximum_a = max(b["rotationErrorDeg"] for b in r["bodies"])
        followup = not r["exactAfterMeasurement"]
        if not followup:
            later = any((q["serverId"], q["limb"]) == key and q["wallMs"] > r["wallMs"] and q["ownerSettled"] and q["followerResting"] and not q["exactAfterMeasurement"] for q in errors)
            if not later:
                problems.append(f"Exact placement lacks later settled observation: {key} at {r['wallMs']}")
        passed = followup and maximum_p < (5 if phase == "settled" else 30) and (phase != "settled" or maximum_a < 10)
        if followup:
            coverage[key].add(phase)
            if not passed:
                problems.append(f"Body target error exceeds {phase} threshold: {key} at {r['wallMs']}")
        body_rows.append({"serverId": key[0], "limb": key[1], "wallMs": r["wallMs"], "phase": phase,
            "maxPositionError": maximum_p, "maxRotationErrorDeg": maximum_a, "passed": passed,
            "exactAfterMeasurement": r["exactAfterMeasurement"], "bodies": r["bodies"]})

    used = set()
    render_coverage = {}
    for f in sorted(renders["follower"], key=lambda r: r["presentationMs"]):
        sid = f["serverId"]
        candidates = [(abs(h["tick"]-f["presentationMs"]), i, h) for i, h in enumerate(renders["host"]) if i not in used and h["serverId"] == sid]
        if not candidates or min(candidates)[0] > 50:
            problems.append(f"Unmatched follower rendered sample: {sid} at {f['presentationMs']}")
            continue
        skew, index, h = min(candidates, key=lambda item: item[:2])
        used.add(index)
        # Body observations are sparse once rested (2 s), while render captures
        # remain frequent. Associate phase using the latest prior follower wall
        # time, not a future result or an older settled state across a new moving
        # or exact-placement event. This labels phase, not this frame's body error.
        matching_errors = [r for r in errors if r["serverId"] == sid and r["limb"] == 0 and r["wallMs"] <= f["wallMs"]]
        if not matching_errors:
            problems.append(f"Rendered sample lacks prior body phase observation: {sid}")
            continue
        state = max(matching_errors, key=lambda r: r["wallMs"])
        if state["exactAfterMeasurement"]:
            problems.append(f"Rendered sample only matched to pre-exact-placement observation: {sid}")
            continue
        phase = "settled" if state["ownerSettled"] and state["followerResting"] else "moving"
        phase_age = f["wallMs"] - state["wallMs"]
        if phase_age > (2100 if phase == "settled" else 100):
            problems.append(f"Rendered sample body phase observation too old: {sid}, {phase_age} ms ({phase})")
            continue
        hb, fb = sorted(h["bones"], key=lambda b:b["bone"]), sorted(f["bones"], key=lambda b:b["bone"])
        if len(hb) != len(fb) or any(a["nameHash"] != b["nameHash"] for a, b in zip(hb, fb)):
            problems.append(f"Rendered bone name/count mismatch: {sid}")
            continue
        bone_errors = [{"bone":a["bone"], "nameHash":a["nameHash"], "positionError":math.dist(a["p"], b["p"]),
            "rotationErrorDeg":angle(quaternion(a["r"]), quaternion(b["r"])), "scaleError":abs(a["scale"]-b["scale"])} for a,b in zip(hb,fb)]
        passed = all(b["positionError"] < (5 if phase == "settled" else 30) and b["scaleError"] < 0.001 and (phase != "settled" or b["rotationErrorDeg"] < 10) for b in bone_errors)
        if not passed:
            problems.append(f"Paired rendered error exceeds {phase} threshold: {sid}")
        render_coverage.setdefault(sid, set()).add(phase)
        render_rows.append({"serverId":sid, "phase":phase, "hostTick":h["tick"], "followerPresentationMs":f["presentationMs"],
            "skewMs":skew, "phaseObservationWallMs":state["wallMs"], "phaseObservationAgeMs":phase_age,
            "phaseAssociation":"latest prior body observation; not a paired physics measurement at this rendered frame",
            "nonAtomic":h["nonAtomic"] or f["nonAtomic"], "passed":passed, "bones":bone_errors})
    if len(used) != len(renders["host"]):
        problems.append("Unmatched host rendered samples")
    if not body_rows or not render_rows:
        problems.append("Missing paired body/render evidence")
    for key, phases in coverage.items():
        if phases != {"moving", "settled"}:
            problems.append(f"Missing moving or settled body coverage: {key}")
        if key[1] == 0 and render_coverage.get(key[0], set()) != {"moving", "settled"}:
            problems.append(f"Missing moving or settled rendered coverage: {key[0]}")
    return {"passed":not problems, "scope":"captured samples only; not a continuous-fall or in-game-fix claim",
        "pairedRenderMaxSkewMs":50, "nonAtomicRenderSamples":non_atomic,
        "limits":["Body errors compare follower physics to received/interpolated owner targets, not simultaneously sampled owner bodies.",
            "Paired render is nearest unused owner tick within 50 ms of follower presentation; no interpolation.",
            "Rendered phase uses the latest prior follower body observation: at most 100 ms moving or 2100 ms while both rested, with no intervening moving/exact event. Its body error was measured earlier, not at the rendered frame.",
            "Only host-owned corpse streams are evaluated; opposite-authority records with other server IDs are skipped.",
            "Non-atomic render reads may span native node updates; a sample pass does not prove atomic or continuous parity.",
            "Moving includes follower convergence after owner settlement; literal initial falling is not inferred."],
        "problems":problems, "bodyTargetComparisons":body_rows, "pairedRenderedComparisons":render_rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Host JSONL file or directory")
    parser.add_argument("--follower", required=True, help="Follower JSONL file or directory")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    host, hp = read_capture(args.host)
    follower, fp = read_capture(args.follower)
    result = compare(host, follower, hp+fp)
    Path(args.output).write_text(json.dumps(result, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    print(json.dumps({"passed":result["passed"], "problems":result["problems"], "output":args.output}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
