#!/usr/bin/env bash
# Fetch the diskette images the 88-DCDD cabinet's manifest lists. All five
# come from Mike Douglas's collection (dhansel/Altair8800, pinned to one
# commit so this can't drift), verified by SHA-256.
#
#   cpm63k.dsk     CP/M 2.2 -- freely redistributable since 2022 (DRDOS Inc. /
#                  Bryan Sparks); the disk the "CP/M Workstation" preset boots.
#   games.dsk      CP/M game disk -- 1970s type-in BASIC (Star Trek, Lunar
#                  Lander) plus small hobbyist CP/M games; decades of open,
#                  unchallenged redistribution, no asserted rights holder.
#   altairdos.dsk  MITS Altair DOS 1.0 -- MITS ceased operating in 1979;
#                  orphaned, same long-unchallenged category CP/M itself sat
#                  in before its 2001 release, just never formally released.
#   wordstar.dsk   WordStar 3.0 -- commercial (MicroPro); no known public
#                  release, current rights holder unclear.
#   zork1.dsk      Zork I -- Activision (now Microsoft) is an active rights
#                  holder, though Infocom titles including this one were
#                  released as promotional freeware in the mid-1990s.
#
# The last two carry real, differentiated redistribution risk -- see
# ALTAIR_REVIEW.md-adjacent discussion in the repo history. Fetched anyway by
# owner decision; this comment is here so that decision stays a visible,
# deliberate one and not a silent default.
#
#   ./fetch-disks.sh            # fetch what's missing / wrong
#   ./fetch-disks.sh --force

set -euo pipefail
cd "$(dirname "$0")"

BASE="https://raw.githubusercontent.com/dhansel/Altair8800/8b0ac49448144f0afed1d5108559dc8507268a85/disks"

# name        source file    sha256
DISKS='
cpm63k.dsk     DISK01.DSK  730a806ae374c99d8c1ee1c4ab83b674ea7773050300c7d7aa4bfbd29752e4f2
games.dsk      DISK05.DSK  346f9c5e45d76b952258112bab108df930f6b90ff37ff9f93d27a4773cfcaba8
wordstar.dsk   DISK07.DSK  a0f83eb26932ce4f5fa8f6eb17bfbe210aa21c540aeecacbf24b022d1c790cdf
zork1.dsk      DISK08.DSK  1680b45fc6da42718967b97a2897840274f39027c6f45cd749db805ba48ed05e
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
