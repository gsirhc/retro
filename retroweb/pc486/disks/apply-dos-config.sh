#!/usr/bin/env bash
# Replace the FreeDOS installer's own FDCONFIG.SYS/FDAUTO.BAT with a single,
# plain MS-DOS-style boot config: no menu, and no V86 memory manager. HIMEMX
# alone gives the plain XMS that DOS/16M and DOS/4GW extenders ask for, with
# nothing virtualising memory underneath them -- the installer's default
# JEMMEX NOEMS provides no VCPI, and PC486_REVIEW.md §19.4 records a second
# extender faulting under that manager too. DOS=HIGH still leaves ~620KB
# conventional free, which is room enough for a game and its sound driver.
# The CD-ROM and mouse drivers are what a period game needs to install and
# run, and nothing else is loaded. See PC486_REVIEW.md.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: apply-dos-config.sh <freedos-hdd.img>" >&2
    exit 1
fi
HDD=$1

if [ ! -f "$HDD" ]; then
    echo "apply-dos-config: $HDD not found" >&2
    exit 1
fi
if ! command -v mcopy >/dev/null 2>&1; then
    echo "apply-dos-config: mtools not installed (brew install mtools)" >&2
    exit 1
fi

# mtools addresses a partition inside an image with the @@<byte offset>
# suffix; the single partition's start LBA is in the MBR's first entry.
PART_OFFSET=$(( $(od -An -t u4 -j 454 -N 4 "$HDD" | tr -d ' ') * 512 ))

# mtools refuses a disk image whose geometry it cannot infer; the images here
# are plain sector dumps, which is exactly the case this skips the check for.
export MTOOLS_SKIP_CHECK=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# CRLF: DOS's own TYPE renders a bare LF as one unbroken line.
printf 'DOS=HIGH\r\nFILES=40\r\nBUFFERS=20\r\nLASTDRIVE=Z\r\nDEVICE=C:\\FreeDOS\\BIN\\HIMEMX.EXE\r\nDEVICE=C:\\FreeDOS\\BIN\\UDVD2.SYS /D:FDCD0001\r\nSHELL=C:\\FreeDOS\\BIN\\COMMAND.COM C:\\FreeDOS\\BIN /E:1024 /P=C:\\FDAUTO.BAT\r\n' > "$tmp/FDCONFIG.SYS"

printf '@ECHO OFF\r\nSET DOSDIR=C:\\FreeDOS\r\nSET PATH=C:\\FreeDOS\\BIN\r\nSET TEMP=C:\\FreeDOS\\TEMP\r\nSET BLASTER=A220 I5 D1 H5 T6 P330\r\nC:\\FreeDOS\\BIN\\SHSUCDX.COM /D:FDCD0001\r\nC:\\FreeDOS\\BIN\\CTMOUSE.EXE\r\nPROMPT $P$G\r\n' > "$tmp/FDAUTO.BAT"

mcopy -o -i "$HDD@@$PART_OFFSET" "$tmp/FDCONFIG.SYS" "::FDCONFIG.SYS"
mcopy -o -i "$HDD@@$PART_OFFSET" "$tmp/FDAUTO.BAT" "::FDAUTO.BAT"

echo "apply-dos-config: wrote FDCONFIG.SYS and FDAUTO.BAT to $HDD"
