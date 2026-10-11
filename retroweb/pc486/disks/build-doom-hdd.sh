#!/usr/bin/env bash
# DOOM preset C:: the factory image's MBR and boot sector over a fresh FAT16 with only
# the kernel, FreeCOM, HIMEMX and CTMOUSE, then id's own installer run onto it by
# build_doom_hdd, then an AUTOEXEC.BAT that starts DOOM.
set -euo pipefail

if [ $# -ne 7 ]; then
    echo "usage: build-doom-hdd.sh <build_doom_hdd> <bios> <vgabios> <freedos-hdd.img> <doom-1.img> <doom-2.img> <out.img>" >&2
    exit 1
fi
TOOL=$1 BIOS=$2 VGA=$3 SRC=$4 DISK1=$5 DISK2=$6 OUT=$7
command -v mformat >/dev/null 2>&1 || { echo "build-doom-hdd: mtools not installed (brew install mtools)" >&2; exit 1; }

export MTOOLS_SKIP_CHECK=1
PART_LBA=$(od -An -t u4 -j 454 -N 4 "$SRC" | tr -d ' ')
PART_SECTORS=$(od -An -t u4 -j 458 -N 4 "$SRC" | tr -d ' ')
PART_OFFSET=$(( PART_LBA * 512 ))
S="$SRC@@$PART_OFFSET"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
BASE="$tmp/base.img"
D="$BASE@@$PART_OFFSET"

dd if="$SRC" of="$BASE" bs=512 count="$PART_LBA" status=none
dd if=/dev/null of="$BASE" bs=1 seek="$(wc -c < "$SRC" | tr -d ' ')" status=none
dd if="$SRC" of="$tmp/vbr.bin" bs=512 skip="$PART_LBA" count=1 status=none

# Same BPB as the installer's FORMAT left on the factory C: (minfo)
mformat -i "$D" -T "$PART_SECTORS" -h 9 -s 55 -H "$PART_LBA" -c 8 -r 32 \
    -v DOOM -N 1993D00D -B "$tmp/vbr.bin" ::

mcopy -m -i "$S" ::KERNEL.SYS ::COMMAND.COM ::FREEDOS/BIN/HIMEMX.EXE ::FREEDOS/BIN/CTMOUSE.EXE "$tmp/"
printf 'DOS=HIGH\r\nFILES=40\r\nBUFFERS=20\r\nDEVICE=C:\\DOS\\HIMEMX.EXE\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > "$tmp/CONFIG.SYS"
AUTOEXEC='@ECHO OFF\r\nPATH C:\\DOS\r\nSET BLASTER=A220 I5 D1 H5 T6 P330\r\nC:\\DOS\\CTMOUSE.EXE\r\nPROMPT $P$G\r\n'
printf "$AUTOEXEC" > "$tmp/AUTOEXEC.BAT"
touch -t 199401010000 "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT"
mcopy -m -i "$D" "$tmp/KERNEL.SYS" "$tmp/COMMAND.COM" "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT" ::
mmd -i "$D" ::DOS
mcopy -m -i "$D" "$tmp/HIMEMX.EXE" "$tmp/CTMOUSE.EXE" ::DOS

"$TOOL" "$BIOS" "$VGA" "$BASE" "$DISK1" "$DISK2" "$OUT"

printf "${AUTOEXEC}CD \\\\DOOMS\r\nDOOM\r\n" > "$tmp/AUTOEXEC.BAT"
touch -t 199401010000 "$tmp/AUTOEXEC.BAT"
mcopy -o -m -i "$OUT@@$PART_OFFSET" "$tmp/AUTOEXEC.BAT" ::AUTOEXEC.BAT
python3 "$(dirname "$0")/zero-free-clusters.py" "$OUT" "$PART_OFFSET"

echo "build-doom-hdd: wrote $OUT"
