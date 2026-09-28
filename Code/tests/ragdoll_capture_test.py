"""Meaningful offline capture gates; no game process or mod build."""
import importlib.util
import copy
import json
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).resolve().parents[2] / "Tools/InGameTests/Compare-RagdollCapture.py"
spec = importlib.util.spec_from_file_location("ragdoll_capture", path)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
baseline_path = path.with_name("Measure-RagdollBaseline.py")
baseline_spec = importlib.util.spec_from_file_location("ragdoll_baseline", baseline_path)
baseline = importlib.util.module_from_spec(baseline_spec)
baseline_spec.loader.exec_module(baseline)


def render(tick, remote):
    return {"kind":"ragdoll_render", "id":1, "serverId":42, "tick":tick,
        "presentationMs":tick, "wallMs":tick+5000, "remote":remote,
        "stateFlags":2 << 21, "boneCount":1, "truncated":False, "nonAtomic":True,
        "bones":[{"bone":0, "nameHash":9876, "nameComplete":True,
                  "p":[0,0,0], "r":[1,0,0,0,1,0,0,0,1], "scale":1}]}


def error(tick, settled):
    return {"kind":"ragdoll_error", "id":1, "serverId":42, "limb":0,
        "tick":tick, "presentationMs":tick, "wallMs":tick+5000, "sampleAgeMs":20,
        "ownerSettled":settled, "followerResting":settled, "exactAfterMeasurement":False,
        "phase":"post-solver-before-correction", "dropped":0, "bodyCount":1, "truncated":False,
        "bodies":[{"body":0, "positionError":1, "rotationErrorDeg":0, "p":[1,0,0],
                   "targetP":[0,0,0], "q":[0,0,0,1], "targetQ":[0,0,0,1], "motion":2, "ownerMotion":2}]}


class CaptureComparison(unittest.TestCase):
    def rows(self):
        return [render(1000, False), render(2000, False)], [error(1000, False), error(2000, True), render(1000, True), render(2000, True)]

    def test_complete_moving_and_settled_pass_as_observations_only(self):
        result = capture.compare(*self.rows())
        self.assertTrue(result["passed"], result["problems"])
        self.assertEqual(result["nonAtomicRenderSamples"], 4)
        self.assertIn("not a continuous-fall", result["scope"])
        self.assertEqual(len(result["bodyTargetComparisons"]), 2)
        self.assertEqual(len(result["pairedRenderedComparisons"]), 2)

    def test_strict_body_thresholds_and_recomputed_error(self):
        for index, value in [(0, 30), (1, 5)]:
            host, follower = self.rows()
            follower[index]["bodies"][0].update(positionError=value, p=[value,0,0])
            self.assertFalse(capture.compare(host, follower)["passed"])
        host, follower = self.rows()
        follower[1]["bodies"][0]["p"] = [80,0,0]  # dishonest residual cannot pass
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_rotated_settled_render_fails_even_if_bodies_match(self):
        host, follower = self.rows()
        follower[-1]["bones"][0]["r"] = [0,-1,0,1,0,0,0,0,1]
        result = capture.compare(host, follower)
        self.assertFalse(result["passed"])
        self.assertAlmostEqual(result["pairedRenderedComparisons"][-1]["bones"][0]["rotationErrorDeg"], 90)

    def test_cannot_pass_without_both_phases_or_both_channels(self):
        host, follower = self.rows()
        for h, f in [(host[:1], [follower[0], follower[2]]), (host[1:], [follower[1], follower[3]]),
                     ([], follower[:2]), (host, follower[2:]), ([], [])]:
            self.assertFalse(capture.compare(h, f)["passed"])

    def test_stale_drop_gap_truncated_and_missing_names_fail(self):
        for key, value in [("sampleAgeMs",101), ("dropped",1), ("truncated",True), ("bodyCount",2)]:
            host, follower = self.rows()
            follower[0][key] = value
            self.assertFalse(capture.compare(host, follower)["passed"], key)
        host, follower = self.rows()
        self.assertFalse(capture.compare(host, follower+[{"kind":"ragdoll_capture_gap", "dropped":1}])["passed"])
        for key, value in [("nameComplete",False), ("nameHash",777)]:
            host, follower = self.rows()
            follower[-1]["bones"][0][key] = value
            self.assertFalse(capture.compare(host, follower)["passed"])

    def test_render_count_duplicate_indices_and_bad_scale_fail(self):
        for change in (lambda r:r.update(boneCount=2), lambda r:r.update(truncated=True),
                       lambda r:r["bones"][0].update(scale=1.1),
                       lambda r:r.update(boneCount=2, bones=r["bones"]*2)):
            host, follower = self.rows()
            change(follower[-1])
            self.assertFalse(capture.compare(host, follower)["passed"])

    def test_keyframing_only_when_owner_engine_type_matches(self):
        host, follower = self.rows()
        follower[0]["bodies"][0]["motion"] = 4
        self.assertFalse(capture.compare(host, follower)["passed"])
        follower[0]["bodies"][0]["ownerMotion"] = 4
        self.assertTrue(capture.compare(host, follower)["passed"])

    def test_exact_placement_requires_later_measured_settle(self):
        host, follower = self.rows()
        follower[1]["exactAfterMeasurement"] = True
        self.assertFalse(capture.compare(host, follower)["passed"])
        # Large pre-placement error is retained as evidence, not counted as the
        # later settled result. A separate later body/render sample must pass.
        follower[1]["bodies"][0].update(positionError=80, p=[80,0,0])
        host[1] = render(2200, False)
        follower[-1] = render(2200, True)
        follower.append(error(2200, True))
        result = capture.compare(host, follower)
        self.assertTrue(result["passed"], result["problems"])
        self.assertFalse(result["bodyTargetComparisons"][1]["passed"])

    def test_nearest_pair_skew_and_unused_samples_are_explicit(self):
        host, follower = self.rows()
        host[0]["tick"] += 50
        self.assertTrue(capture.compare(host, follower)["passed"])
        host[0]["tick"] += 1
        self.assertFalse(capture.compare(host, follower)["passed"])
        host, follower = self.rows()
        self.assertFalse(capture.compare(host+[render(3000, False)], follower)["passed"])

    def test_nonfinite_and_invalid_rotations_rejected(self):
        for value in (float("nan"), float("inf")):
            host, follower = self.rows()
            follower[0]["bodies"][0]["positionError"] = value
            self.assertFalse(capture.compare(host, follower)["passed"])
        for matrix in ([0]*9, [-1,0,0,0,1,0,0,0,1]):
            host, follower = self.rows()
            follower[-1]["bones"][0]["r"] = matrix
            self.assertFalse(capture.compare(host, follower)["passed"])

    def test_living_actors_cannot_supply_missing_corpse_evidence(self):
        host, follower = self.rows()
        host[1]["stateFlags"] = follower[-1]["stateFlags"] = 0
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_settled_phase_covers_two_second_observation_cadence(self):
        host, follower = self.rows()
        host.extend([render(2700, False), render(4100, False)])
        follower.extend([render(2700, True), render(4100, True)])
        result = capture.compare(host, follower)
        self.assertTrue(result["passed"], result["problems"])
        self.assertEqual(result["pairedRenderedComparisons"][-1]["phaseObservationAgeMs"], 2100)
        self.assertIn("not a paired physics", result["pairedRenderedComparisons"][-1]["phaseAssociation"])
        host[-1] = render(4101, False)
        follower[-1] = render(4101, True)
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_moving_phase_allows_only_one_hundred_ms_prior_observation(self):
        host, follower = self.rows()
        host.append(render(1100, False))
        follower.append(render(1100, True))
        self.assertTrue(capture.compare(host, follower)["passed"])
        host[-1] = render(1101, False)
        follower[-1] = render(1101, True)
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_intervening_moving_or_exact_event_invalidates_old_rest_phase(self):
        for exact in (False, True):
            host, follower = self.rows()
            host.append(render(3300, False))
            follower.append(render(3300, True))
            newer = error(3000, exact)
            newer["exactAfterMeasurement"] = exact
            follower.append(newer)
            result = capture.compare(host, follower)
            self.assertFalse(result["passed"])
            self.assertFalse(any(r["followerPresentationMs"] == 3300 for r in result["pairedRenderedComparisons"]))

    def test_opposite_authority_corpses_skipped_unless_server_id_conflicts(self):
        host, follower = self.rows()
        other_host, other_follower = render(1500, True), render(1500, False)
        other_error = error(1500, True)
        for r in (other_host, other_follower, other_error):
            r["serverId"] = 99
        host.extend([other_host, other_error])
        follower.append(other_follower)
        self.assertTrue(capture.compare(host, follower)["passed"])
        other_follower["serverId"] = 42
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_stationary_owner_target_age_does_not_reject_follower_convergence(self):
        host, follower = self.rows()
        follower[0].update(ownerSettled=True, followerResting=False, sampleAgeMs=5000)
        self.assertTrue(capture.compare(host, follower)["passed"])
        follower[0]["ownerSettled"] = False
        self.assertFalse(capture.compare(host, follower)["passed"])

    def test_jsonl_directory_and_malformed_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"capture.jsonl"
            path.write_text(json.dumps(render(1000, False))+"\n{broken\n", encoding="utf-8")
            records, problems = capture.read_capture(directory)
            self.assertEqual(len(records), 1)
            self.assertEqual(len(problems), 1)
            self.assertFalse(capture.compare(*self.rows(), problems)["passed"])


class RootFrameClosure(unittest.TestCase):
    def pair(self):
        identity = [1,0,0,0,1,0,0,0,1]
        quarter_turn = [0,-1,0,1,0,0,0,0,1]
        host = {"renderRoot":{"readable":True, "worldT":[0,0,0]},
                "renderLocalSamples":[{"index":0,"t":[0,0,0],"r":identity}],
                "renderWorldSamples":[{"index":0,"t":[0,0,0],"r":identity,"s":1},
                                      {"index":1,"t":[2,0,0],"r":identity,"s":1}]}
        follower = copy.deepcopy(host)
        follower["renderRoot"]["worldT"] = [10,20,30]
        follower["renderWorldSamples"] = [
            {"index":0,"t":[10,20,30],"r":quarter_turn,"s":1},
            {"index":1,"t":[10,22,30],"r":quarter_turn,"s":1}]
        return host, follower

    def test_root_frame_explains_rigid_offset_without_fitting_bones(self):
        result = baseline.root_closure(*self.pair())
        self.assertAlmostEqual(result["rootRotationErrorDegrees"], 90)
        self.assertEqual(result["articulatedSummary"]["count"], 1)
        self.assertEqual(result["rootRelativeSummary"]["positionErrorGameUnits"]["max"], 0)
        self.assertEqual(result["rootRelativeSummary"]["rotationErrorDegrees"]["max"], 0)

    def test_root_frame_does_not_hide_an_articulated_error(self):
        host, follower = self.pair()
        follower["renderWorldSamples"][1]["t"][1] += 7
        result = baseline.root_closure(host, follower)
        self.assertAlmostEqual(result["articulatedSummary"]["positionErrorGameUnits"]["max"], 7)

    def test_root_frame_refuses_non_root_anchor(self):
        host, follower = self.pair()
        follower["renderLocalSamples"][0]["t"][2] = 1
        self.assertIsNone(baseline.root_closure(host, follower))


if __name__ == "__main__":
    unittest.main()
