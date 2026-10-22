#!/usr/bin/env bash
# Fetch the freely-licensed AT BIOS this machine substitutes for IBM's
# copyrighted one (Bochs Project, LGPL) and the fonts egabios/
# assembles into its EGA BIOS. Pinned to commits and SHA-256 verified. Files
# land next to this script.
#
#   ./fetch-bios.sh            # fetch what's missing / wrong
#   ./fetch-bios.sh --force    # re-fetch everything
#
# Bochs rather than SeaBIOS: SeaBIOS needs an i386 cross compiler and ships no
# prebuilt binaries. BIOS-bochs-legacy is a prebuilt plain-ISA/no-PCI BIOS in the
# Bochs tree. See IBM_PCAT_REVIEW.md §6.
#
# Both BIOSes are compatible stand-ins, not the genuine firmware.

set -euo pipefail
cd "$(dirname "$0")"

FORCE="${1:-}"

# Pinned to bochs-emu/Bochs@ff17a0c2bbabccf96d33af4e08ba8061889b079d (master, fetched 2026-09-05).
BASE="https://raw.githubusercontent.com/bochs-emu/Bochs/ff17a0c2bbabccf96d33af4e08ba8061889b079d/bochs/bios"

# Yossi Gil's font collection (fntcol16.zip, Simtel msdos/screen) for egabios/. Two Simtel mirrors, same bytes.
FONTS="https://ftp.icm.edu.pl/pub/simtelnet/msdos/screen/fntcol16.zip https://ftp.sunet.se/mirror/archive/ftp.sunet.se/pub/simtelnet/msdos/screen/fntcol16.zip"

# name  |  url  |  sha256
FILES=(
  "BIOS-bochs-legacy|$BASE/BIOS-bochs-legacy|d8848f08e6c832144d906d2c2175469a818a54062d42e8c7e60895b9e9aa930c"
  "fntcol16.zip|$FONTS|ee875bb96a5be0cb70f87e1fc3ce85aa4c0c1ed78f958bb8160f9023a25d32a6"
)

sha() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

for row in "${FILES[@]}"; do
  IFS='|' read -r name url want <<<"$row"
  if [ -z "$FORCE" ] && [ -f "$name" ] && [ "$(sha "$name")" = "$want" ]; then
    echo "ok    $name"
    continue
  fi
  echo "fetch $name"
  tmp="$(mktemp)"
  fetched=
  for u in $url; do
    if curl -sSfL --retry 3 -o "$tmp" "$u"; then fetched=1; break; fi
  done
  [ -n "$fetched" ] || { echo "  could not fetch $name" >&2; rm -f "$tmp"; exit 1; }
  got="$(sha "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "  SHA-256 mismatch for $name" >&2
    echo "  want $want" >&2
    echo "  got  $got" >&2
    rm -f "$tmp"
    exit 1
  fi
  mv "$tmp" "$name"
  chmod 644 "$name"
done

echo "BIOS + EGA BIOS fonts ready."
