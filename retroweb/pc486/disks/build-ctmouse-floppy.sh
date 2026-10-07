#!/usr/bin/env bash
# Build the CuteMouse driver floppy for the Drivers panel: a 1.44MB FAT12 image
# with CTMOUSE.EXE and its two diagnostics.
# The files come from the FreeDOS HDD image this machine already builds
# (disks/freedos-hdd.img, from the verified FD13-LiveCD zip; see
# fetch-freedos-cd.sh), not a separate download:
#   - no second external dependency: CuteMouse's SourceForge download serves an
#     HTML interstitial and the ibiblio paths 404
#   - the bytes inherit the zip's published SHA-256
#   - no new licensing question (GPL, redistributed by FreeDOS)
# CTMOUSE.EXE is checked against the SHA-256 of the official CuteMouse 2.1b4
# release binary, so a different build fails loudly. A floppy because the HDD image
# is the machine as it left the factory and drivers arrived on disks, and it
# leaves the user's saved hard disk alone.
set -euo pipefail

cd "$(dirname "$0")"

HDD=freedos-hdd.img
OUT=ctmouse.img
# CuteMouse 2.1b4's ctmouse.exe, the build FreeDOS 1.3 packages.
CTMOUSE_SHA=822cf550c9e19a22785722d2306aa08ede10ff20bfe931d5a81a15f77c5f363e
# CTMOUSE.EXE sits in BIN; its two diagnostics in the BIN/CTMOUSE subdirectory
BIN_DIR="::/FREEDOS/BIN"
TOOL_DIR="::/FREEDOS/BIN/CTMOUSE"

if [ ! -f "$HDD" ]; then
    echo "build-ctmouse-floppy: $HDD not found -- run 'make hdd-image' first" >&2
    exit 1
fi
if ! command -v mformat >/dev/null 2>&1; then
    echo "build-ctmouse-floppy: mtools not installed (brew install mtools)" >&2
    exit 1
fi

# mtools can't infer geometry for plain sector dumps; this skips the check
export MTOOLS_SKIP_CHECK=1

# mtools addresses a partition with @@<byte offset>; the start LBA is in the MBR's first entry
PART_OFFSET=$(( $(od -An -t u4 -j 454 -N 4 "$HDD" | tr -d ' ') * 512 ))

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mtype -i "$HDD@@$PART_OFFSET" "$BIN_DIR/CTMOUSE.EXE"   > "$tmp/CTMOUSE.EXE"
mtype -i "$HDD@@$PART_OFFSET" "$TOOL_DIR/MOUSETST.COM" > "$tmp/MOUSETST.COM"
mtype -i "$HDD@@$PART_OFFSET" "$TOOL_DIR/PROTOCOL.COM" > "$tmp/PROTOCOL.COM"

got=$(shasum -a 256 "$tmp/CTMOUSE.EXE" | cut -d' ' -f1)
if [ "$got" != "$CTMOUSE_SHA" ]; then
    echo "build-ctmouse-floppy: CTMOUSE.EXE checksum mismatch" >&2
    echo "  want $CTMOUSE_SHA" >&2
    echo "  got  $got" >&2
    exit 1
fi

# CRLF: DOS's own TYPE renders a bare LF as one unbroken line.
printf 'CuteMouse 2.1b4 -- DOS mouse driver (INT 33h) for this machine'\''s PS/2 mouse.\r\n\r\nA DOS program reaches the mouse through INT 33h, which is a driver, not\r\nfirmware -- and POST leaves the AUX port disabled, exactly as a real 486\r\ndoes. So nothing sees the mouse until this is loaded.\r\n\r\n  A:\\CTMOUSE /P      load it; /P skips the serial-port probe\r\n  A:\\MOUSETST        confirm the driver sees movement\r\n  A:\\PROTOCOL        report which protocol was detected\r\n  A:\\CTMOUSE /U      unload\r\n\r\nTo load it at every boot, copy CTMOUSE.EXE to C: and add a CTMOUSE /P\r\nline to AUTOEXEC.BAT.\r\n\r\nIn the emulator: tick "Capture mouse", then CLICK the screen to take\r\npointer lock -- without lock the page sends no movement at all. Your own\r\nEsc key releases the lock instead of reaching DOS, so use the Esc button\r\nin the Function & extended keys panel.\r\n\r\nCuteMouse is GPL software, taken from the FreeDOS distribution this\r\nmachine already ships.\r\n' > "$tmp/README.TXT"

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=512 count=2880 status=none
mformat -i "$OUT" -f 1440 -v CTMOUSE ::
for f in CTMOUSE.EXE MOUSETST.COM PROTOCOL.COM README.TXT; do
    mcopy -i "$OUT" "$tmp/$f" "::$f"
done

echo "build-ctmouse-floppy: wrote $OUT"
mdir -i "$OUT" ::
