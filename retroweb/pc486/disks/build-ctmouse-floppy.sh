#!/usr/bin/env bash
# Build the CuteMouse driver floppy this machine offers from its Drivers
# panel: a 1.44MB FAT12 image carrying CTMOUSE.EXE and its two diagnostics.
#
# The files are extracted from the FreeDOS hard-disk image this machine
# already builds (disks/freedos-hdd.img, itself assembled by a real installer
# run from the verified FD13-LiveCD zip -- see fetch-freedos-cd.sh), NOT
# downloaded separately. That matters for three reasons:
#
#   - No second external dependency to rot. CuteMouse's own SourceForge
#     download serves an HTML interstitial to anything without a browser
#     session, and the FreeDOS ibiblio paths for it 404, so a pinned URL
#     here would be a build that breaks on someone else's machine.
#   - The bytes are already verified. Whatever CTMOUSE.EXE the FreeDOS image
#     carries came through that zip's published SHA-256, so it inherits the
#     same provenance as everything else on the image.
#   - No new licensing question. CuteMouse is GPL and FreeDOS redistributes
#     it as a standard package; taking it from the distribution this machine
#     already ships keeps that story unchanged.
#
# The extracted CTMOUSE.EXE is checked against the SHA-256 of the official
# CuteMouse 2.1b4 release binary, so a FreeDOS image that ever carried a
# different build fails the build loudly instead of shipping silently.
#
# Why a floppy rather than putting it on the hard disk: the HDD image is the
# machine "as it left the factory" and a driver disk is what period software
# actually arrived on -- and it leaves the user's own saved hard disk alone.
set -euo pipefail

cd "$(dirname "$0")"

HDD=freedos-hdd.img
OUT=ctmouse.img
# CuteMouse 2.1b4's ctmouse.exe, the build FreeDOS 1.3 packages.
CTMOUSE_SHA=822cf550c9e19a22785722d2306aa08ede10ff20bfe931d5a81a15f77c5f363e
# The FreeDOS image's single partition starts at LBA 63 (see its MBR); mtools
# addresses a partition inside an image with the @@<byte offset> suffix.
PART_OFFSET=32256
# CTMOUSE.EXE sits in BIN itself; its two diagnostics in the BIN/CTMOUSE
# subdirectory alongside the localized builds.
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

# mtools refuses a disk image whose geometry it cannot infer; the images here
# are plain sector dumps, which is exactly the case this skips the check for.
export MTOOLS_SKIP_CHECK=1

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
