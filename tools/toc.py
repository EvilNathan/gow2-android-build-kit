#!/usr/bin/env python
"""Inspect and extract files described by GODOFWAR.TOC.

Usage:
  python -I tools/toc.py <GODOFWAR.TOC> list
  python -I tools/toc.py <GODOFWAR.TOC> find <text>
  python -I tools/toc.py <GODOFWAR.TOC> extract <name> <output>

The TOC lives in the disc root. PART1.PAK and PART2.PAK must be in the same
folder when extracting. Offsets in the TOC are (partition, sector) pairs; the
PAK files themselves are sector-aligned, so the file offset is sector * 2048.
"""
import os
import struct
import sys

SECTOR = 2048


def read_toc(path):
    with open(path, "rb") as handle:
        data = handle.read()
    count = struct.unpack_from("<I", data, 0)[0]
    table = 4 + count * 36
    entries = []
    for index in range(count):
        base = 4 + index * 36
        name = data[base:base + 24].split(b"\0", 1)[0].decode("latin-1")
        size, encounters, table_index = struct.unpack_from("<III", data, base + 24)
        word = struct.unpack_from("<I", data, table + table_index * 4)[0]
        entries.append((name, size, encounters, word))
    return entries


def describe(entry):
    name, size, encounters, word = entry
    partition, sector = divmod(word, 10_000_000)
    return "%-24s %10d bytes  encounters=%-3d pak=%d sector=%-8d offset=0x%x" % (
        name, size, encounters, partition + 1, sector, sector * SECTOR)


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 1
    toc_path = sys.argv[1]
    command = sys.argv[2]
    entries = read_toc(toc_path)
    if command == "list":
        for entry in entries:
            print(describe(entry))
        return 0
    if command == "find":
        if len(sys.argv) < 4:
            print("find needs a search string", file=sys.stderr)
            return 1
        needle = sys.argv[3].lower()
        for entry in entries:
            if needle in entry[0].lower():
                print(describe(entry))
        return 0
    if command == "extract":
        if len(sys.argv) < 5:
            print("extract needs a TOC name and output path", file=sys.stderr)
            return 1
        wanted = sys.argv[3]
        match = next((e for e in entries if e[0].lower() == wanted.lower()), None)
        if match is None:
            print("not found: %s" % wanted, file=sys.stderr)
            return 1
        name, size, _encounters, word = match
        partition, sector = divmod(word, 10_000_000)
        if partition > 1:
            print("unsupported partition %d" % (partition + 1), file=sys.stderr)
            return 1
        pak_name = "PART%d.PAK" % (partition + 1)
        pak_path = os.path.join(os.path.dirname(os.path.abspath(toc_path)), pak_name)
        if not os.path.exists(pak_path):
            print("missing %s" % pak_path, file=sys.stderr)
            return 1
        with open(pak_path, "rb") as source, open(sys.argv[4], "wb") as target:
            source.seek(sector * SECTOR)
            remaining = size
            while remaining:
                chunk = source.read(min(remaining, 1024 * 1024))
                if not chunk:
                    raise SystemExit("short read in %s" % pak_path)
                target.write(chunk)
                remaining -= len(chunk)
        print("wrote %s (%d bytes, %s)" % (sys.argv[4], size, name))
        return 0
    print("unknown command: %s" % command, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
