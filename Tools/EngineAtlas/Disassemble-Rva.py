#!/usr/bin/env python3
"""Read-only x64 disassembly of a bounded RVA in an installed PE image."""

import argparse
from pathlib import Path

import capstone
import pefile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--rva", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--bytes", type=int, default=96)
    args = parser.parse_args()
    if not 1 <= args.bytes <= 256:
        parser.error("--bytes must be 1..256")
    image = pefile.PE(str(args.exe), fast_load=True)
    section = image.get_section_by_rva(args.rva)
    if section is None or not section.Characteristics & 0x20000000:
        parser.error("RVA is not in an executable section")
    code = image.get_data(args.rva, args.bytes)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for instruction in decoder.disasm(code, image.OPTIONAL_HEADER.ImageBase + args.rva):
        print(f"{instruction.address:#018x}  {instruction.mnemonic:<8} {instruction.op_str}")


if __name__ == "__main__":
    main()
