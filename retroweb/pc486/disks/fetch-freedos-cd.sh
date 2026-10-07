#!/usr/bin/env bash
# Fetch FreeDOS 1.3's official LiveCD ISO, this machine's default CD-ROM disc,
# from ibiblio.org (same source as fetch-freedos.sh). FD13-LiveCD.zip holds
# FD13LIVE.iso (lowercase extension inside the zip), FD13BOOT.img (a 1.44MB
# floppy to boot machines that can't El Torito, which then installs from the CD)
# and readme.txt (not extracted). Verified against the zip's published SHA-256
# (verify.txt), since ibiblio has no per-file checksums.
#
#   ./fetch-freedos-cd.sh            # fetch what's missing / wrong
#   ./fetch-freedos-cd.sh --force    # re-fetch

set -euo pipefail
cd "$(dirname "$0")"
FORCE="${1:-}"

URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-LiveCD.zip"
ZIP_SHA="250d3980b38d988ddfe100df1a5d09009c6fee17cbabd17274d5284e02a491c4"
ISO_NAME="freedos-cd.iso"
BOOT_NAME="freedos-boot.img"

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }

if [ -z "$FORCE" ] && [ -f "$ISO_NAME" ] && [ -f "$BOOT_NAME" ]; then
  echo "ok    $ISO_NAME, $BOOT_NAME (already fetched -- pass --force to re-verify/re-fetch)"
  exit 0
fi
echo "fetch $ISO_NAME + $BOOT_NAME (via FD13-LiveCD.zip)"
tmp="$(mktemp -d)"
curl -sSfL --retry 3 -o "$tmp/fd.zip" "$URL"
got_zip="$(sha "$tmp/fd.zip")"
if [ "$got_zip" != "$ZIP_SHA" ]; then
  echo "  SHA-256 mismatch for FD13-LiveCD.zip" >&2
  echo "  want $ZIP_SHA" >&2
  echo "  got  $got_zip" >&2
  echo "  (re-check against https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/verify.txt --" >&2
  echo "   this value was transcribed once, not machine-verified against a second independent source)" >&2
  rm -rf "$tmp"
  exit 1
fi
unzip -o -q "$tmp/fd.zip" "FD13LIVE.iso" "FD13BOOT.img" -d "$tmp/x"
mv "$tmp/x/FD13LIVE.iso" "$ISO_NAME"
mv "$tmp/x/FD13BOOT.img" "$BOOT_NAME"
chmod 644 "$ISO_NAME" "$BOOT_NAME"
rm -rf "$tmp"
echo "FreeDOS LiveCD ISO + boot floppy ready ($ISO_NAME, $BOOT_NAME)."
