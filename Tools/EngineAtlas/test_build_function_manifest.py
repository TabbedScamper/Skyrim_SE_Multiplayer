import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "engine_atlas_manifest", Path(__file__).with_name("Build-FunctionManifest.py")
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
boundary_spec = importlib.util.spec_from_file_location(
    "engine_atlas_boundary", Path(__file__).with_name("Inspect-FunctionBoundary.py")
)
boundary = importlib.util.module_from_spec(boundary_spec)
boundary_spec.loader.exec_module(boundary)


class AddressLibraryV5Tests(unittest.TestCase):
    def test_sparse_id_to_rva_table(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "versionlib.bin"
            path.write_bytes(
                module.HEADER.pack(5, 1, 7, 104, 0, b"SkyrimSE.exe", 8, 0, 4)
                + struct.pack("<4I", 0, 0x1234, 0, 0xABCD)
            )
            header, mapping = module.read_v5(path)
            self.assertEqual(header["version"], "1.7.104.0")
            self.assertEqual(header["module"], "SkyrimSE.exe")
            self.assertEqual(mapping, {1: 0x1234, 3: 0xABCD})

    def test_rejects_inconsistent_count(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "versionlib.bin"
            path.write_bytes(module.HEADER.pack(5, 1, 7, 104, 0, b"SkyrimSE.exe", 8, 0, 2)
                             + struct.pack("<I", 0x1234))
            with self.assertRaisesRegex(ValueError, "count does not match"):
                module.read_v5(path)

    def test_rejects_old_format(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "versionlib.bin"
            path.write_bytes(module.HEADER.pack(2, 1, 7, 104, 0, b"SkyrimSE.exe", 8, 0, 0))
            with self.assertRaisesRegex(ValueError, "format 5"):
                module.read_v5(path)


class PEFunctionBoundaryTests(unittest.TestCase):
    def test_start_inside_end_and_missing(self):
        functions = [(0x100, 0x140, 0x900), (0x200, 0x220, 0x920)]
        self.assertEqual(boundary.classify(0x100, functions)["status"], "pdata-start")
        self.assertEqual(boundary.classify(0x110, functions)["offset_into_function"], 0x10)
        self.assertEqual(boundary.classify(0x140, functions)["status"], "no-pdata-entry")
        self.assertEqual(boundary.classify(0x200, functions)["status"], "pdata-start")


if __name__ == "__main__":
    unittest.main()
