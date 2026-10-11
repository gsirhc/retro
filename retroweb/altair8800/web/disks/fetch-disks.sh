#!/usr/bin/env bash
# Fetch the 88-DCDD diskette images from Mike Douglas's collection (dhansel/Altair8800,
# pinned to one commit), verified by SHA-256.
#   cpm63k.dsk     CP/M 2.2, redistributable since 2022 (http://cpm.z80.de/license.html)
#   games.dsk      type-in BASIC and CP/M games, long open redistribution
#   altairdos.dsk  MITS Altair DOS 1.0, orphaned since MITS closed in 1979
#   diskbasic.dsk, basicprg.dsk, wordstar.dsk, supercalc.dsk, zork.dsk,
#   cpm3boot.dsk, cpm3util.dsk  see README.md for each one's license status
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
diskbasic.dsk  DISK03.DSK  d93f8659e34cb5d5a1898455654bb836449339c4ad1796cf89c93951151c3e17
basicprg.dsk   DISK04.DSK  18281e590f4872ad8fcbeeaf26a2100129732816b384620e60535135a73fae46
supercalc.dsk  DISK06.DSK  fbf5018bc5d15e3d38e156749bd05791a6fe724bf8bb65490729185570901129
wordstar.dsk   DISK07.DSK  a0f83eb26932ce4f5fa8f6eb17bfbe210aa21c540aeecacbf24b022d1c790cdf
zork.dsk       DISK08.DSK  1680b45fc6da42718967b97a2897840274f39027c6f45cd749db805ba48ed05e
cpm3boot.dsk   DISK13.DSK  940c76efc6fc053f6006dc1b75e49ea19d75bbe08005ee10f03f9dd62fae8ce4
cpm3util.dsk   DISK14.DSK  f1540150ce18bbb0848cca3a0ef021785d75820799c24dca2cdbc0c29396cd39
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
