"""Offline capture-checker tests; no build and no game changes."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[2] / "Tools/InGameTests/Compare-WorldAnimation.py"
spec = importlib.util.spec_from_file_location("wall_compare", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Comparison(unittest.TestCase):
    def rows(self):
        return [{"name": f"Chunk{i:02}", "Host": "1,2,3,1", "Follower": "1,2,3,1"} for i in range(2, 13)]

    def test_all_eleven_required(self):
        self.assertTrue(module.compare(self.rows())["passed"])
        self.assertFalse(module.compare(self.rows()[:-1])["passed"])

    def test_one_bad_chunk_fails(self):
        rows = self.rows()
        rows[7]["Follower"] = "1,2,5,1"
        self.assertFalse(module.compare(rows)["passed"])

    def test_missing_scale_nonfinite_and_duplicates_fail(self):
        for value in ("1,2,3", "1,2,nan,1", "1,2,3,2"):
            rows = self.rows(); rows[0]["Follower"] = value
            self.assertFalse(module.compare(rows)["passed"])
        rows = self.rows()
        self.assertFalse(module.compare(rows + [rows[0]])["passed"])

    def test_root_and_stage_cannot_substitute_for_chunks(self):
        self.assertFalse(module.compare([{"name": "Chunk01", "Host": "1,2,3,1", "Follower": "1,2,3,1", "stage": 103}])["passed"])


if __name__ == "__main__":
    unittest.main()
