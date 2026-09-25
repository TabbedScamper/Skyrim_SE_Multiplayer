#!/usr/bin/env python3
"""Classify candidate RVAs against the x64 PE exception (.pdata) table.

A .pdata start is stronger boundary evidence than an Address Library lookup,
but it does not establish the symbol name, ABI, or safe call phase. Leaf
functions can legitimately lack a RUNTIME_FUNCTION entry.
"""

import argparse
import bisect
import importlib.util
import json
import struct
from pathlib import Path


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_runtime_functions(path: Path):
    data = path.read_bytes()
    if len(data) < 0x40:
        raise ValueError("truncated DOS header")
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    coff = pe + 4
    if u16(data, coff) != 0x8664:
        raise ValueError("expected x64 PE machine")
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    if u16(data, optional) != 0x20B:
        raise ValueError("expected PE32+ optional header")
    if optional_size < 112 + 4 * 8 or u32(data, optional + 108) <= 3:
        raise ValueError("missing exception data directory")
    directory = optional + 112 + 3 * 8
    exception_rva, exception_size = struct.unpack_from("<II", data, directory)
    if exception_size == 0 or exception_size % 12:
        raise ValueError("invalid exception directory size")

    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        off = section_table + index * 40
        virtual_size, virtual_rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((virtual_rva, max(virtual_size, raw_size), raw_ptr, raw_size))

    def file_offset(rva, length):
        for start, virtual_size, raw_ptr, raw_size in sections:
            delta = rva - start
            if 0 <= delta and delta + length <= min(virtual_size, raw_size):
                pos = raw_ptr + delta
                if pos + length <= len(data):
                    return pos
        raise ValueError(f"RVA 0x{rva:X} is outside file-backed PE sections")

    start = file_offset(exception_rva, exception_size)
    functions = [struct.unpack_from("<III", data, start + i)
                 for i in range(0, exception_size, 12)]
    if any(begin >= end for begin, end, _ in functions):
        raise ValueError("invalid RUNTIME_FUNCTION range")
    functions.sort(key=lambda row: row[0])
    return functions


def classify(rva, functions):
    begins = [row[0] for row in functions]
    index = bisect.bisect_right(begins, rva) - 1
    if index < 0 or rva >= functions[index][1]:
        return {"status": "no-pdata-entry"}
    begin, end, unwind = functions[index]
    return {
        "status": "pdata-start" if rva == begin else "pdata-inside",
        "function_begin_rva": begin,
        "function_end_rva": end,
        "unwind_rva": unwind,
        "offset_into_function": rva - begin,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--addrlib", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--id", dest="ids", action="append", type=int, default=[])
    parser.add_argument("--rva", dest="rvas", action="append", type=lambda value: int(value, 0), default=[])
    args = parser.parse_args()
    if not args.ids and not args.rvas:
        parser.error("provide at least one --id or --rva")

    atlas_path = Path(__file__).with_name("Build-FunctionManifest.py")
    spec = importlib.util.spec_from_file_location("engine_atlas_manifest", atlas_path)
    atlas = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(atlas)
    header, mapping = atlas.read_v5(args.addrlib)
    if header["module"].casefold() != args.exe.name.casefold():
        raise ValueError("EXE and Address Library module names differ")
    functions = read_runtime_functions(args.exe)
    labels = {}
    if args.manifest:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if manifest["exe"]["sha256"] != atlas.sha256(args.exe):
            raise ValueError("manifest EXE hash differs from current EXE")
        if manifest["address_library"]["sha256"] != header["sha256"]:
            raise ValueError("manifest Address Library hash differs")
        for symbol in manifest["symbols"]:
            labels.setdefault(symbol["id"], []).append(symbol["name"])
    for ident in args.ids:
        rva = mapping.get(ident)
        result = {"id": ident, "rva": rva, "candidate_names": labels.get(ident, [])}
        result.update(classify(rva, functions) if rva is not None
                      else {"status": "id-absent"})
        print(json.dumps(result, sort_keys=True))
    for rva in args.rvas:
        result = {"rva": rva}
        result.update(classify(rva, functions))
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
