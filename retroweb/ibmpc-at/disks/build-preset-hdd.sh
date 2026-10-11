#!/usr/bin/env bash
# Preset C: (?disk=<preset>): the factory image's MBR and boot sector over a fresh FAT16
# with only the kernel and FreeCOM, then the game's own installer run onto it by
# build_preset_hdd, then a CONFIG.SYS that loads the game as the shell. With 640K and
# no extended memory, FreeCOM stays 69K resident and Keen 1 runs out of memory under it.
set -euo pipefail

if [ $# -ne 7 ]; then
    echo "usage: build-preset-hdd.sh <keen1> <build_preset_hdd> <bios> <egabios> <freedos-hdd.img> <out.img> <disk1.img>" >&2
    exit 1
fi
PRESET=$1 TOOL=$2 BIOS=$3 VGA=$4 SRC=$5 OUT=$6 DISK1=$7
case "$PRESET" in
    keen1) LABEL=KEEN SERIAL=1990C33E GAME='C:\\KEEN1.EXE' ;;
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
mformat -i "$D" -T "$PART_SECTORS" -h 5 -s 17 -H "$PART_LBA" -c 4 -r 32 \
    -v "$LABEL" -N "$SERIAL" -B "$tmp/vbr.bin" ::

mcopy -m -i "$S" ::KERNEL.SYS ::COMMAND.COM "$tmp/"
printf 'FILES=20\r\nBUFFERS=20\r\nSHELL=C:\\COMMAND.COM C:\\ /E:1024 /P\r\n' > "$tmp/CONFIG.SYS"
AUTOEXEC='@ECHO OFF\r\nPROMPT $P$G\r\n'
printf "$AUTOEXEC" > "$tmp/AUTOEXEC.BAT"
touch -t 199001010000 "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT"
mcopy -m -i "$D" "$tmp/KERNEL.SYS" "$tmp/COMMAND.COM" "$tmp/CONFIG.SYS" "$tmp/AUTOEXEC.BAT" ::

"$TOOL" "$PRESET" "$BIOS" "$VGA" "$BASE" "$OUT" "$DISK1"

printf "FILES=15\r\nBUFFERS=10\r\nSHELL=${GAME}\r\n" > "$tmp/CONFIG.SYS"
touch -t 199001010000 "$tmp/CONFIG.SYS"
mcopy -o -m -i "$OUT@@$PART_OFFSET" "$tmp/CONFIG.SYS" ::CONFIG.SYS
python3 "$(dirname "$0")/../../pc486/disks/zero-free-clusters.py" "$OUT" "$PART_OFFSET"

echo "build-preset-hdd: wrote $OUT"
