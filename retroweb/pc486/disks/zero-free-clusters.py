#!/usr/bin/env python3
"""Zero every free cluster of a FAT16 partition so deleted files don't bloat the gzip."""
import struct
import sys

path, part_offset = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(part_offset)
    bs = f.read(512)
    bps, spc, reserved, nfats, root_entries, small, _, spf = struct.unpack_from("<HBHBHHBH", bs, 11)
    total = small or struct.unpack_from("<I", bs, 32)[0]
    if bs[54:62] != b"FAT16   ":
        sys.exit("zero-free-clusters: not FAT16")
    f.seek(part_offset + reserved * bps)
    fat = f.read(spf * bps)
    data_start = reserved + nfats * spf + (root_entries * 32 + bps - 1) // bps
    clusters = (total - data_start) // spc
    blank = bytes(spc * bps)
    zeroed = 0
    for n in range(2, clusters + 2):
        if struct.unpack_from("<H", fat, n * 2)[0] == 0:
            f.seek(part_offset + (data_start + (n - 2) * spc) * bps)
            f.write(blank)
            zeroed += 1
print(f"zero-free-clusters: {zeroed} free clusters zeroed")
