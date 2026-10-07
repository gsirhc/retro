#!/usr/bin/env bash
# Test-only: builds a C: as the 504MB builds had it (1024/16/63, one FAT16 partition at LBA 63,
# 8KB clusters, 512 root entries) with every file of the shipped image plus C:\LEGACY\MARKER.TXT.
# --full adds a file too big for the 256MB drive.
# usage: make-legacy-hdd.sh [--full] <freedos-hdd.img> <out.img>
set -euo pipefail

full=0
if [ "${1:-}" = "--full" ]; then full=1; shift; fi
if [ $# -ne 2 ]; then
    echo "usage: make-legacy-hdd.sh [--full] <freedos-hdd.img> <out.img>" >&2
    exit 1
fi
SRC=$1
OUT=$2
export MTOOLS_SKIP_CHECK=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
SRC_OFF=$(( $(od -An -t u4 -j 454 -N 4 "$SRC" | tr -d ' ') * 512 ))
IMG=$tmp/legacy.img
OFF=$(( 63 * 512 ))

dd if=/dev/zero of="$IMG" bs=512 count=0 seek=1032192 2>/dev/null
# MBR: shipped boot code plus the 504MB image's partition entry (LBA 63, 1,032,129 sectors).
dd if="$SRC" of="$IMG" bs=446 count=1 conv=notrunc 2>/dev/null
printf '\x80\x01\x01\x00\x06\x0f\x3f\x00\x3f\x00\x00\x00\xc1\xbf\x0f\x00' |
    dd of="$IMG" bs=1 seek=446 conv=notrunc 2>/dev/null
printf '\x55\xaa' | dd of="$IMG" bs=1 seek=510 conv=notrunc 2>/dev/null

dd if="$SRC" of="$tmp/boot.bin" bs=512 skip=$(( SRC_OFF / 512 )) count=1 2>/dev/null
mformat -i "$IMG@@$OFF" -T 1032129 -h 16 -s 63 -H 63 -c 16 -r 32 -B "$tmp/boot.bin" -v FREEDOS2022 ::

mkdir "$tmp/tree"
mcopy -s -m -i "$SRC@@$SRC_OFF" "::/*" "$tmp/tree/"
mkdir "$tmp/tree/LEGACY"
printf 'CONVERTED FROM 504MB\r\n' > "$tmp/tree/LEGACY/MARKER.TXT"
if [ $full -eq 1 ]; then
    dd if=/dev/zero of="$tmp/tree/LEGACY/BIG.DAT" bs=1048576 count=245 2>/dev/null
fi
# KERNEL.SYS and the config files first, as the installer placed them.
for f in KERNEL.SYS COMMAND.COM FDCONFIG.SYS FDAUTO.BAT; do
    if [ -f "$tmp/tree/$f" ]; then
        mcopy -m -i "$IMG@@$OFF" "$tmp/tree/$f" "::/$f"
        rm "$tmp/tree/$f"
    fi
done
mcopy -s -m -i "$IMG@@$OFF" "$tmp/tree/"* "::/"

mkdir -p "$(dirname "$OUT")"
mv "$IMG" "$OUT"
echo "make-legacy-hdd: wrote $OUT"
