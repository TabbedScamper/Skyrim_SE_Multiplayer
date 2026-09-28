"""Qualify the eleven moving wall chunks in an existing paired capture.

Input is the wall-comparison.json array (name, Host, Follower). This checks
the captured XYZ/scale components, not unrecorded rotations or Havok membership.
Missing, duplicate and nonfinite data fail. Stage 103 is never a success signal.
"""
import argparse
import json
import math
from pathlib import Path


def compare(rows, position_tolerance=0.05, scale_tolerance=0.0001):
    expected = {f"Chunk{i:02d}" for i in range(2, 13)}
    seen = set()
    results, errors = [], []
    for row in rows:
        name = row.get("name")
        if name not in expected:
            continue
        if name in seen:
            errors.append(f"duplicate {name}")
            continue
        seen.add(name)
        try:
            sides = []
            for side in ("Host", "Follower"):
                value = row[side]
                numbers = [float(x) for x in (value.split(",") if isinstance(value, str) else value)]
                if len(numbers) != 4 or not all(math.isfinite(x) for x in numbers):
                    raise ValueError("expected finite XYZ and scale")
                sides.append(numbers)
            host, follower = sides
            distance = math.dist(host[:3], follower[:3])
            scale = abs(host[3] - follower[3])
            results.append({"name": name, "positionError": distance, "scaleError": scale,
                            "matches": distance <= position_tolerance and scale <= scale_tolerance})
        except (KeyError, ValueError, TypeError) as error:
            errors.append(f"{name}: {error}")
    errors.extend(f"missing {name}" for name in sorted(expected - seen))
    return {"passed": len(results) == 11 and not errors and all(r["matches"] for r in results),
            "checked": len(results), "positionTolerance": position_tolerance, "scaleTolerance": scale_tolerance,
            "chunks": results, "errors": errors,
            "scope": "captured XYZ and scale only; passage/collision and unrecorded rotation need separate observations"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("comparison", type=Path)
    parser.add_argument("--position-tolerance", type=float, default=0.05)
    parser.add_argument("--scale-tolerance", type=float, default=0.0001)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    for tolerance in (args.position_tolerance, args.scale_tolerance):
        if not math.isfinite(tolerance) or tolerance < 0:
            parser.error("tolerances must be finite and nonnegative")
    result = compare(json.loads(args.comparison.read_text(encoding="utf-8-sig")), args.position_tolerance, args.scale_tolerance)
    output = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(output + "\n", encoding="utf-8")
    print(output)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
