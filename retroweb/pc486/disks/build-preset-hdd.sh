#!/usr/bin/env bash
# Preset C: (?disk=<preset>): the factory image's MBR and boot sector over a fresh FAT16
# with only the kernel, FreeCOM, HIMEMX and CTMOUSE, then the game's own installer run
# onto it by build_preset_hdd, then an AUTOEXEC.BAT that starts the game.
set -euo pipefail

if [ $# -lt 7 ]; then
    echo "usage: build-preset-hdd.sh <doom|wolf3d> <build_preset_hdd> <bios> <vgabios> <freedos-hdd.img> <out.img> <disk1.img> [disk2.img]" >&2
    exit 1
fi
PRESET=$1 TOOL=$2 BIOS=$3 VGA=$4 SRC=$5 OUT=$6
shift 6
case "$PRESET" in
    doom)   LABEL=DOOM   SERIAL=1993D00D RUN='CD \\DOOMS\r\nDOOM\r\n' ;;
    wolf3d) LABEL=WOLF3D SERIAL=1992F3D0 RUN='CD \\WOLF1\r\nWOLF3D\r\n' ;;
    *) echo "build-preset-hdd: unknown preset $PRESET" >&2; exit 1 ;;
esac
command -v mformat >/dev/null 2>&1 || { echo "build-preset-hdd: mtools not installed (brew install mtools)" >&2; exit 1; }

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
    -v "$LABEL" -N "$SERIAL" -B "$tmp/vbr.bin" ::

mcopy -m -i "$S" ::KERNEL.SYS ::COMMAND.COM ::FREEDOS/BIN/HIMEMX.EXE ::FREEDOS/BIN/CTMOUSE.EXE "$tmp/"
printf 'DOS=HIGH\r\nFILES=40\r\nBUFFERS=20\r\nDEVICE=C:\\DOS\\HIMEMX.EXE\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > "$tmp/CONFIG.SYS"
AUTOEXEC='@ECHO OFF\r\nPATH C:\\DOS\r\nSET BLASTER=A220 I5 D1 H5 T6 P330\r\nC:\\DOS\\CTMOUSE.EXE\r\nPROMPT $P$G\r\n'
printf "$AUTOEXEC" > "$tmp/AUTOEXEC.BAT"
touch -t 199401010000 "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT"
mcopy -m -i "$D" "$tmp/KERNEL.SYS" "$tmp/COMMAND.COM" "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT" ::
mmd -i "$D" ::DOS
mcopy -m -i "$D" "$tmp/HIMEMX.EXE" "$tmp/CTMOUSE.EXE" ::DOS

"$TOOL" "$PRESET" "$BIOS" "$VGA" "$BASE" "$OUT" "$@"

printf "${AUTOEXEC}${RUN}" > "$tmp/AUTOEXEC.BAT"
touch -t 199401010000 "$tmp/AUTOEXEC.BAT"
mcopy -o -m -i "$OUT@@$PART_OFFSET" "$tmp/AUTOEXEC.BAT" ::AUTOEXEC.BAT
python3 "$(dirname "$0")/zero-free-clusters.py" "$OUT" "$PART_OFFSET"

echo "build-preset-hdd: wrote $OUT"
