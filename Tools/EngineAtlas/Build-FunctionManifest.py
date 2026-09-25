#!/usr/bin/env python3
"""Read-only CommonLib symbol candidates for Address Library v5.

The v5 layout follows alandtse/CommonLibSSE-NG REL/IDDB.h::header_v5_t:
format/version, 64-byte module name, pointer size, data format, count,
then one uint32 RVA per ID. The older Code/client/VersionDb.h::LoadV5
reads pointer size and data format together as one uint64.
This does not validate that any label, prototype, or field layout is correct.
"""

import argparse
import hashlib
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path


HEADER = struct.Struct("<i4i64siii")


def read_v5(path: Path):
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError("truncated Address Library v5 header")
    fmt, major, minor, revision, build, module, pointer_size, data_format, count = HEADER.unpack_from(raw)
    if fmt != 5:
        raise ValueError(f"expected Address Library format 5, found {fmt}")
    if pointer_size != 8:
        raise ValueError(f"expected 64-bit pointer size 8, found {pointer_size}")
    if data_format != 0:
        raise ValueError(f"unexpected Address Library v5 data format {data_format}")
    if count < 0:
        raise ValueError("negative Address Library v5 count")
    if len(raw) != HEADER.size + count * 4:
        raise ValueError("Address Library v5 count does not match file size")
    module_name = module.split(b"\0", 1)[0].decode("ascii", errors="strict")
    if not module_name.lower().endswith(".exe"):
        raise ValueError(f"unexpected module name: {module_name!r}")
    offsets = struct.unpack_from(f"<{count}I", raw, HEADER.size)
    mapping = {i: rva for i, rva in enumerate(offsets) if rva}
    return {
        "version": f"{major}.{minor}.{revision}.{build}",
        "module": module_name,
        "pointer_size": pointer_size,
        "data_format": data_format,
        "address_count": count,
        "nonzero_count": len(mapping),
        "sha256": hashlib.sha256(raw).hexdigest().upper(),
    }, mapping


def sha256(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolkit-scripts", type=Path, required=True)
    parser.add_argument("--commonlib", type=Path, required=True,
                        help="CommonLibSSE-NG root (scans include/ and src/)")
    parser.add_argument("--commonlib-commit", required=True,
                        help="exact source commit, obtained with git rev-parse HEAD")
    parser.add_argument("--addrlib", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--expected-version", default="1.7.104.0")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(args.toolkit_scripts.resolve()))
    from mine_function_ids import normalize_name, scan_file  # noqa: E402

    header, id_to_rva = read_v5(args.addrlib)
    if header["version"] != args.expected_version:
        raise ValueError(f"version mismatch: {header['version']} != {args.expected_version}")
    if header["module"].casefold() != args.exe.name.casefold():
        raise ValueError(f"module mismatch: {header['module']} != {args.exe.name}")

    entries = []
    seen = set()
    scanned_files = 0
    unresolved = 0
    for root in (args.commonlib / "include", args.commonlib / "src"):
        if not root.is_dir():
            raise ValueError(f"missing source directory: {root}")
        for source in sorted((*root.rglob("*.h"), *root.rglob("*.cpp"))):
            scanned_files += 1
            for symbol in scan_file(source, str(source.relative_to(root))):
                name = normalize_name(symbol["name"])
                ident = symbol["ae_id"]
                key = (name, symbol["kind"], ident)
                if not name or name.endswith("::") or key in seen:
                    continue
                seen.add(key)
                if not ident or ident not in id_to_rva:
                    unresolved += 1
                    continue
                entries.append({
                    "name": name,
                    "kind": symbol["kind"],
                    "id": ident,
                    "rva": id_to_rva[ident],
                    "source": symbol["src"],
                    "confidence": "candidate-address-only",
                })

    by_id = defaultdict(set)
    for entry in entries:
        by_id[entry["id"]].add(entry["name"])
    ambiguous = {str(ident): sorted(names) for ident, names in by_id.items()
                 if len(names) > 1}
    manifest = {
        "schema": 1,
        "caution": "Address Library resolution only; symbol names and ABI need disassembly/live validation",
        "exe": {"name": args.exe.name, "sha256": sha256(args.exe)},
        "address_library": header,
        "commonlib_commit": args.commonlib_commit,
        "scanned_files": scanned_files,
        "unresolved_symbols": unresolved,
        "ambiguous_ids": ambiguous,
        "symbols": sorted(entries, key=lambda e: (e["id"], e["name"])),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{len(entries)} candidate labels, {len(ambiguous)} ambiguous IDs, "
          f"{unresolved} unresolved symbols -> {args.out}")


if __name__ == "__main__":
    main()
