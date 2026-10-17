#!/usr/bin/env bash
# Fetch the shareware releases the PC disk libraries ship, as their authors
# distributed them, verified by SHA-256 into cache/.
#   1keen.zip    Commander Keen 1 shareware, Apogee's BBS release (DEICE set)
#   1wolf14.zip  Wolfenstein 3-D 1.4 shareware, id/Apogee (DEICE set)
#   doom19s.zip  DOOM 1.9 shareware, id's two-disk install set
#   3dduke13.zip Duke Nukem 3D 1.3D shareware, 3D Realms' own release
#   tyrian21.zip Tyrian 2.1, reported freeware since 2004 (README.md, Provenance)
#   ./fetch-shareware.sh            # fetch what's missing / wrong
#   ./fetch-shareware.sh --force

set -euo pipefail
cd "$(dirname "$0")"
mkdir -p cache

# name         sha256                                                            url
FILES='
1keen.zip    db2cca0b3623f9dbe9be8b84d3825541238b0f9ade65069e385387b9946c0be1  https://image.dosgamesarchive.com/games/1keen.zip
1wolf14.zip  7755d3e6cc8897e9628fa1c13fe24d9657d901af2c3793ae258381431dcc69f2  https://www.gamers.org/pub/games/wolf3d/official/1wolf14.zip
doom19s.zip  cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6  https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/doom/doom19s.zip
3dduke13.zip c67efd179022bc6d9bde54f404c707cbcbdc15423c20be72e277bc2bdddf3d0e  https://archive.org/download/3dduke13/3dduke13.zip
tyrian21.zip 7790d09a2a3addcd33c66ef063d5900eb81cc9c342f4807eb8356364dd1d9277  https://camanis.net/tyrian/tyrian21.zip
'

sha() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

force="${1:-}"
echo "$FILES" | while read -r name want url; do
  [ -z "$name" ] && continue
  out="cache/$name"
  if [ "$force" = "" ] && [ -f "$out" ] && [ "$(sha "$out")" = "$want" ]; then
    echo "ok    $name"
    continue
  fi
  echo "fetch $name"
  tmp="$(mktemp)"
  curl -sSfL --retry 3 -o "$tmp" "$url"
  got="$(sha "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "  SHA-256 mismatch for $name: want $want got $got" >&2
    rm -f "$tmp"; exit 1
  fi
  mv "$tmp" "$out"
  chmod 644 "$out"
done
echo "Shareware ready."
