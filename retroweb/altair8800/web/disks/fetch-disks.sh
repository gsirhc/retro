#!/usr/bin/env bash
# Fetch the 88-DCDD diskette images from Mike Douglas's collection (dhansel/Altair8800,
# pinned to one commit), verified by SHA-256.
#   cpm63k.dsk     CP/M 2.2, redistributable since 2022 (http://cpm.z80.de/license.html)
#   games.dsk      type-in BASIC and CP/M games, long open redistribution
#   altairdos.dsk  MITS Altair DOS 1.0, orphaned since MITS closed in 1979
#   ./fetch-disks.sh            # fetch what's missing / wrong
#   ./fetch-disks.sh --force

set -euo pipefail
cd "$(dirname "$0")"

BASE="https://raw.githubusercontent.com/dhansel/Altair8800/8b0ac49448144f0afed1d5108559dc8507268a85/disks"

# name        source file    sha256
DISKS='
cpm63k.dsk     DISK01.DSK  730a806ae374c99d8c1ee1c4ab83b674ea7773050300c7d7aa4bfbd29752e4f2
games.dsk      DISK05.DSK  346f9c5e45d76b952258112bab108df930f6b90ff37ff9f93d27a4773cfcaba8
altairdos.dsk  DISK02.DSK  2a1c26ec4a8add6fedd78585556b5d0c25829705a7a0be5a3897fe4516dbb8d7
'

sha() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

force="${1:-}"
echo "$DISKS" | while read -r name src want; do
  [ -z "$name" ] && continue
  if [ "$force" = "" ] && [ -f "$name" ] && [ "$(sha "$name")" = "$want" ]; then
    echo "ok    $name"
    continue
  fi
  echo "fetch $name"
  tmp="$(mktemp)"
  curl -sSfL --retry 3 -o "$tmp" "$BASE/$src"
  got="$(sha "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "  SHA-256 mismatch for $name: want $want got $got" >&2
    rm -f "$tmp"; exit 1
  fi
  mv "$tmp" "$name"
  chmod 644 "$name"
done
echo "Diskette images ready."
