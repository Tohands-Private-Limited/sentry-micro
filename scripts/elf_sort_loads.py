#!/usr/bin/env python3
"""
Sort an ELF's PT_LOAD program headers by virtual address, so Sentry finds its load address.

Sentry's symbolicator (getsentry/symbolic, `symbolic-debuginfo/src/elf.rs`) takes an ELF's
load address to be the `p_vaddr` of the *first* PT_LOAD entry in the program header table,
in table order, not the lowest one. It then rebases every symbol and every DWARF address
against it: a function below the load address is dropped, and a DWARF address is taken as
`addr - load_address`, wrapping, so anything that ends up above 4 GiB is dropped as well.
The ELF specification says loadable segments are sorted by `p_vaddr`, which is what makes
"first" and "lowest" the same thing, and most linkers honour it.

Zephyr's ESP32-S3 link does not. Its first PT_LOAD is 36 bytes of RTC memory at 0x50000000,
above every byte of code, so taken as is the load address would be 0x50000000 and not a
single frame would resolve. The lowest PT_LOAD (0x00012820, a NOLOAD placeholder) is what
`elf_info.py` reports as `image_addr` and what the firmware compiles in, and every code
address minus that still fits in 32 bits. Sorting the table makes symbolic agree.

Only the copy that is uploaded to Sentry is sorted, never the ELF the build left behind.
The device runs `zephyr.bin`, which is made from the sections, not from this table; and
leaving the build's own ELF alone keeps `west flash`, `west debug` and the build system's
idea of which outputs are up to date exactly as Zephyr made them.

What is rewritten is the program header table and nothing else: `e_phnum` entries of
`e_phentsize` bytes at `e_phoff`, each one

    32-bit:  p_type p_offset p_vaddr p_paddr p_filesz p_memsz p_flags p_align  (4 bytes each)
    64-bit:  p_type p_flags  p_offset p_vaddr p_paddr p_filesz p_memsz p_align (4,4, then 8)

The PT_LOAD entries are stable-sorted by `p_vaddr` among the slots PT_LOAD entries already
occupy; every other entry (GNU_STACK, PT_NOTE, ...) stays where it was. Entries move whole,
so each segment keeps its own offset, sizes and flags, and the section headers and section
contents are not touched. Running it on an already sorted file changes nothing.

    scripts/elf_sort_loads.py zephyr.elf -o zephyr.sentry.elf
    scripts/elf_sort_loads.py zephyr.sentry.elf --in-place
"""

import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1
# e_phnum at this value means the real count is in section header 0, which only files with
# more than 65534 segments need. Nothing this is meant for comes close, so it is refused
# rather than half-supported.
PN_XNUM = 0xFFFF


class ElfError(ValueError):
    """The input is not an ELF this script can rewrite."""


def _layout(data: bytes):
    """(endian prefix, offset of p_vaddr in an entry, its struct code, e_phoff, entsize, count)."""
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    if data[4] not in (1, 2) or data[5] not in (1, 2):
        raise ElfError("unknown ELF class or byte order")

    is_64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is_64:
        if len(data) < 64:
            raise ElfError("truncated ELF header")
        e_phoff, = struct.unpack_from(endian + "Q", data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x36)
        vaddr_at, vaddr_code, min_entsize = 16, "Q", 56
    else:
        e_phoff, = struct.unpack_from(endian + "I", data, 0x1C)
        e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 0x2A)
        vaddr_at, vaddr_code, min_entsize = 8, "I", 32

    if e_phnum == PN_XNUM:
        raise ElfError("extended program header numbering (PN_XNUM) is not supported")
    if e_phnum and e_phentsize < min_entsize:
        raise ElfError(f"program header entries are {e_phentsize} bytes, expected {min_entsize}")
    if e_phoff + e_phnum * e_phentsize > len(data):
        raise ElfError("program header table runs past the end of the file")
    return endian, vaddr_at, vaddr_code, e_phoff, e_phentsize, e_phnum


def _loads(data: bytes):
    """[(table index, p_vaddr, raw entry bytes)] for every PT_LOAD entry, in table order."""
    endian, vaddr_at, vaddr_code, e_phoff, entsize, count = _layout(data)
    loads = []
    for index in range(count):
        start = e_phoff + index * entsize
        p_type, = struct.unpack_from(endian + "I", data, start)
        if p_type == PT_LOAD:
            vaddr, = struct.unpack_from(endian + vaddr_code, data, start + vaddr_at)
            loads.append((index, vaddr, data[start:start + entsize]))
    return loads


def first_load_vaddr(data: bytes):
    """`p_vaddr` of the first PT_LOAD in table order: what symbolic uses as the load address."""
    loads = _loads(data)
    return loads[0][1] if loads else None


def sort_loads(data: bytes) -> bytes:
    """The same file with its PT_LOAD entries sorted by p_vaddr, everything else unchanged."""
    _, _, _, e_phoff, entsize, _ = _layout(data)
    loads = _loads(data)
    # sorted() is stable, so segments sharing an address keep their relative order and a
    # second run produces the identical table.
    ordered = sorted(loads, key=lambda load: load[1])
    out = bytearray(data)
    for (slot, _, _), (_, _, entry) in zip(loads, ordered):
        start = e_phoff + slot * entsize
        out[start:start + entsize] = entry
    return bytes(out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("elf", type=Path, help="ELF to read")
    parser.add_argument("-o", "--output", type=Path, help="where to write the sorted copy")
    parser.add_argument("--in-place", action="store_true",
                        help="overwrite the input instead of writing a copy")
    args = parser.parse_args(argv)

    if args.in_place == (args.output is not None):
        parser.error("pass exactly one of --output or --in-place")
    if not args.elf.is_file():
        parser.error(f"no such ELF: {args.elf}")
    output = args.elf if args.in_place else args.output
    # In place has to be asked for: sorting the build's own zephyr.elf is exactly what this
    # script exists to avoid, and an output path that happens to name the input is how that
    # would happen by accident.
    if not args.in_place and output.exists() and output.resolve() == args.elf.resolve():
        parser.error(f"{output} is the input; pass --in-place if that is what you mean")

    data = args.elf.read_bytes()
    try:
        before = first_load_vaddr(data)
        sorted_data = sort_loads(data)
    except (ElfError, struct.error) as error:
        print(f"error: {args.elf}: {error}", file=sys.stderr)
        return 1
    if before is None:
        print(f"error: {args.elf} has no PT_LOAD segments", file=sys.stderr)
        return 1
    after = first_load_vaddr(sorted_data)

    if args.in_place and sorted_data == data:
        pass  # Nothing to change; leave the file, and its timestamp, alone.
    else:
        output.write_bytes(sorted_data)
        # A copy of an executable ELF stays executable, like the file it was made from.
        output.chmod(args.elf.stat().st_mode & 0o777)

    state = "already sorted" if sorted_data == data else "sorted"
    print(f"{output.name}: PT_LOAD {state}, first p_vaddr 0x{before:x} -> 0x{after:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
