"""Measure existing paired corpse captures offline; never contacts either game.

Body indices are compared only for matching bone-map checksums and body counts.
Body positions are Havok units (*70); rendered samples are already game units.
Rotation error is the shortest quaternion angle after matrix conversion and
normalization, avoiding float-matrix trace noise close to zero rotation.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import json
import math
from pathlib import Path
import re
import statistics


def quaternion(matrix):
    m = matrix
    trace = m[0] + m[4] + m[8]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = [(m[7] - m[5]) / s, (m[2] - m[6]) / s,
             (m[3] - m[1]) / s, s / 4]
    else:
        i = max(range(3), key=lambda x: m[4 * x])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(max(0, 1 + m[4 * i] - m[4 * j] - m[4 * k])) * 2
        if s == 0:
            raise ValueError("invalid rotation matrix")
        q = [0.0] * 4
        q[i] = s / 4
        q[j] = (m[3 * i + j] + m[3 * j + i]) / s
        q[k] = (m[3 * i + k] + m[3 * k + i]) / s
        q[3] = (m[3 * k + j] - m[3 * j + k]) / s
    length = math.sqrt(sum(x * x for x in q))
    return [x / length for x in q]


def rotation_error(a, b):
    qa, qb = quaternion(a), quaternion(b)
    return math.degrees(2 * math.acos(min(1, abs(sum(x * y for x, y in zip(qa, qb))))))


def body_matrix(t):
    return [t[i] for i in (0, 4, 8, 1, 5, 9, 2, 6, 10)]


def aggregate(errors):
    result = {"count": len(errors)}
    for key in ("positionErrorGameUnits", "rotationErrorDegrees"):
        values = [e[key] for e in errors]
        result[key] = {"mean": statistics.mean(values), "max": max(values)} if values else None
    return result


def root_closure(host, follower):
    """Remove the captured root frame, without fitting the remaining bones.

    This diagnoses a historical sample; it neither corrects nor qualifies it.
    Index zero must be the identity local/root sample on BOTH sides. Duplicate
    root placeholders are reported separately from articulated samples.
    """
    hw = {b["index"]: b for b in host.get("renderWorldSamples", [])}
    fw = {b["index"]: b for b in follower.get("renderWorldSamples", [])}
    hl = {b["index"]: b for b in host.get("renderLocalSamples", [])}
    fl = {b["index"]: b for b in follower.get("renderLocalSamples", [])}
    identity = [1, 0, 0, 0, 1, 0, 0, 0, 1]
    for world, local, actor in ((hw, hl, host), (fw, fl, follower)):
        if 0 not in world or 0 not in local or not actor.get("renderRoot", {}).get("readable"):
            return None
        if (math.dist(local[0]["t"], [0, 0, 0]) > 1e-5 or
                max(abs(x-y) for x, y in zip(local[0]["r"], identity)) > 1e-5 or
                math.dist(world[0]["t"], actor["renderRoot"]["worldT"]) > 0.01 or
                abs(world[0].get("s", 1)-1) > 1e-5):
            return None

    def in_root(bone, root):
        r = root["r"]
        t = [x-y for x, y in zip(bone["t"], root["t"])]
        # NiMatrix3 is row-major: inverse rigid root uses its transpose.
        return ([sum(r[3*k+i]*t[k] for k in range(3)) for i in range(3)],
                [sum(r[3*k+i]*bone["r"][3*k+j] for k in range(3))
                 for i in range(3) for j in range(3)])

    errors = []
    for index in sorted(hw.keys() & fw.keys()):
        a, b = hw[index], fw[index]
        at, ar = in_root(a, hw[0])
        bt, br = in_root(b, fw[0])
        errors.append({"boneIndex": index, "positionErrorGameUnits": math.dist(at, bt),
                       "rotationErrorDegrees": rotation_error(ar, br),
                       "articulated": math.dist(a["t"], hw[0]["t"]) > 0.01 or
                                      math.dist(b["t"], fw[0]["t"]) > 0.01})
    return {"rootPositionErrorGameUnits": math.dist(hw[0]["t"], fw[0]["t"]),
            "rootRotationErrorDegrees": rotation_error(hw[0]["r"], fw[0]["r"]),
            "rootRelativeErrors": errors, "rootRelativeSummary": aggregate(errors),
            "articulatedSummary": aggregate([e for e in errors if e["articulated"]]),
            "scope": "Rigid root-frame closure of sampled nodes only; no node-name identity, writer attribution, or changed-build validation."}


def measure_file(path):
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    animation = data.get("playerAnimation", {})
    if not isinstance(animation, dict):
        return []
    follower = {a["formId"]: a for a in (animation.get("followerPose") or []) if isinstance(a, dict)}
    measured = []
    for host in (animation.get("hostPose") or []):
        if not isinstance(host, dict) or not host.get("dead"):
            continue
        other = follower.get(host["formId"])
        if not other or not other.get("dead"):
            continue
        hr, fr = host.get("ragdoll", {}), other.get("ragdoll", {})
        hb, fb = hr.get("bodySamples", []), fr.get("bodySamples", [])
        complete = bool(hb and fb and len(hb) == hr.get("bodyCount") == len(fb) == fr.get("bodyCount"))
        maps_match = bool(hr.get("boneMapChecksum") and hr.get("boneMapChecksum") == fr.get("boneMapChecksum"))
        if not complete or not maps_match:
            continue
        by_index = {b["index"]: b for b in fb}
        if len(by_index) != len(hb) or any(b["index"] not in by_index for b in hb):
            continue
        errors = []
        for a in hb:
            b = by_index[a["index"]]
            errors.append({"bodyIndex": a["index"],
                "positionErrorGameUnits": 70 * math.dist(a["transform"][12:15], b["transform"][12:15]),
                "rotationErrorDegrees": rotation_error(body_matrix(a["transform"]), body_matrix(b["transform"])),
                "hostMotionType": a["motionType"], "followerMotionType": b["motionType"],
                "hostInWorld": a["inWorld"], "followerInWorld": b["inWorld"],
                "hostLinearSpeedGameUnitsPerSecond": 70 * math.dist(a["linearVelocity"], [0, 0, 0]),
                "followerLinearSpeedGameUnitsPerSecond": 70 * math.dist(b["linearVelocity"], [0, 0, 0]),
                "hostAngularSpeedRadiansPerSecond": math.dist(a["angularVelocity"], [0, 0, 0]),
                "followerAngularSpeedRadiansPerSecond": math.dist(b["angularVelocity"], [0, 0, 0])})
        velocities_rest = all(math.dist(b["linearVelocity"], [0, 0, 0]) < 0.03 and
                              math.dist(b["angularVelocity"], [0, 0, 0]) < 0.05 for b in hb + fb)
        all_in_world = all(b["inWorld"] for b in hb + fb)
        forbidden_keyframing = any(a["motionType"] in (1, 2, 3, 6) and
                                  by_index[a["index"]]["motionType"] == 4 for a in hb)
        phase = "settled_velocity_snapshot" if velocities_rest else "moving_snapshot_fall_not_independently_confirmed"
        if not all_in_world:
            phase = "not_all_bodies_in_world"
        render = []
        render_by_index = {b["index"]: b for b in other.get("renderWorldSamples", [])}
        for a in host.get("renderWorldSamples", []):
            b = render_by_index.get(a["index"])
            if b:
                render.append({"boneIndex": a["index"], "positionErrorGameUnits": math.dist(a["t"], b["t"]),
                               "rotationErrorDegrees": rotation_error(a["r"], b["r"])})
        measured.append({"source": str(path), "capturedAt": data.get("capturedAt"),
            "actorFormId": f"0x{host['formId']:08X}", "bothDead": True, "phase": phase,
            "poseSampleTickDifferenceMs": animation.get("poseSampleTickDifferenceMs"),
            "sampling": data.get("sampling"), "boneMapChecksum": hr.get("boneMapChecksum"),
            "hostConstraintCount": hr.get("constraintCount"), "followerConstraintCount": fr.get("constraintCount"),
            "followerKeyframesOwnerDynamicBodies": forbidden_keyframing,
            "bodyErrors": errors, "bodySummary": aggregate(errors),
            "renderedBoneErrors": render, "renderedBoneSummary": aggregate(render),
            "rootFrameClosure": root_closure(host, other),
            "evaluatedLocalPoseComparison": next((x.get("evaluatedPoseError") for x in animation.get("poseComparisons", [])
                if x.get("formId", "").lower() == f"0x{host['formId']:08x}"), None)})
    return measured


def measure_logs(root):
    expression = re.compile(r"Ragdoll ([0-9A-F]+) limb (\d+):.*?pre-gap ([\d.]+), residual ([\d.]+) u/([\d.]+) deg")
    unique = {}
    settle = {}
    paths = sorted(Path(root).rglob("*.log"))
    for path in paths:
        with path.open(encoding="utf-8", errors="replace") as stream:
            for number, line in enumerate(stream, 1):
                if "Ragdoll " not in line:
                    continue
                match = expression.search(line)
                if match:
                    unique.setdefault(line.strip(), {"source": str(path), "line": number,
                        "actorFormId": match[1], "limb": int(match[2]), "preGapGameUnits": float(match[3]),
                        "postPlacementResidualGameUnits": float(match[4]), "postPlacementResidualDegrees": float(match[5])})
                if "exact owner settle" in line:
                    settle.setdefault(line.strip(), {"source": str(path), "line": number, "text": line.strip()})
    residuals = list(unique.values())
    return {"logFilesRead": len(paths), "uniqueResidualLines": len(residuals),
        "uniqueSettleLines": len(settle), "deduplication": "identical full log lines across copied/rotated logs",
        "interpretation": "Residual is read immediately after SetBodyPose. This is write-back accuracy, not pre-correction or paired rendered-bone parity. Exact owner settle records the stream flag, not independent follower rest.",
        "maxReportedPreGapGameUnits": max((r["preGapGameUnits"] for r in residuals), default=None),
        "maxReportedPostPlacementResidualGameUnits": max((r["postPlacementResidualGameUnits"] for r in residuals), default=None),
        "maxReportedPostPlacementResidualDegrees": max((r["postPlacementResidualDegrees"] for r in residuals), default=None),
        "firstResidualEvidence": residuals[:4], "firstSettleEvidence": list(settle.values())[:4]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", default="C:/Users/mwalt/SkyrimSeamlessCoop/Tools/InGameTests/artifacts")
    parser.add_argument("--captures", default="C:/Tools/skyrim_re/agent/captures")
    parser.add_argument("--output", default="docs/reverse-engineering/ragdoll-baseline-20260927.json")
    parser.add_argument("--workers", type=int, default=32)
    args = parser.parse_args()
    paths = sorted(Path(args.artifacts).glob("authority-*.json"))
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        samples = [sample for result in pool.map(measure_file, map(str, paths)) for sample in result]
    result = {"schema": 1, "method": "Offline paired captures; normalized quaternion shortest arc; body translation *70; rendered translation already game units.",
        "authorityFilesRead": len(paths), "workerProcesses": args.workers,
        "limits": ["Historical captures span different builds; they are not measurements of the changed build or necessarily commit 25fa030d.",
            "Matching body indices/counts and bone-map checksum establish correspondence; they are not rendered skeleton bone indices.",
            "Snapshot sample ticks are not proof of identical presentation time. Moving does not prove initial fall; no continuous fall trajectory is available.",
            "Settled means both sampled velocity sets below 0.03 Havok units/s and 0.05 rad/s; this does not prove sustained rest.",
            "A keyframed follower of dynamic owner bodies violates the owner rule even when pose error is zero; it is not dynamic-steering success.",
            "Rendered samples are bounded subsets of bones, not complete skeletons; padded/unmapped bone samples may repeat root transforms.",
            "No game, process attachment, harness run, deployment, or in-game validation was performed."],
        "samples": samples, "logs": measure_logs(args.captures)}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"Read {len(paths)} authority captures; measured {len(samples)} paired dead-actor snapshots; wrote {output}")
    for sample in samples:
        print(Path(sample["source"]).name, sample["actorFormId"], sample["phase"],
              "body", sample["bodySummary"], "render", sample["renderedBoneSummary"])
    print("Log summary", {k: v for k, v in result["logs"].items() if not k.endswith("Evidence")})


if __name__ == "__main__":
    main()
