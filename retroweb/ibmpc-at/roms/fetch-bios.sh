#!/usr/bin/env bash
# Fetch the freely-licensed firmware this machine substitutes for IBM's
# copyrighted AT BIOS and EGA video BIOS. Both are from the Bochs Project
# (LGPL), pinned to a commit and SHA-256 verified. Files land next to this script.
#
#   ./fetch-bios.sh            # fetch what's missing / wrong
#   ./fetch-bios.sh --force    # re-fetch everything
#
# Bochs rather than SeaBIOS: SeaBIOS needs an i386 cross compiler and ships no
# prebuilt binaries. BIOS-bochs-legacy is a prebuilt plain-ISA/no-PCI BIOS in the
# Bochs tree, as is VGABIOS-lgpl-latest.bin. See IBM_PCAT_REVIEW.md §6.
#
# Both are compatible stand-ins, not the genuine firmware. The VGA BIOS offers
# more modes than a real EGA; the EGA device enforces the 640x350x16 ceiling.

set -euo pipefail
cd "$(dirname "$0")"

FORCE="${1:-}"

# Pinned to bochs-emu/Bochs@ff17a0c2bbabccf96d33af4e08ba8061889b079d (master, fetched 2026-09-05).
BASE="https://raw.githubusercontent.com/bochs-emu/Bochs/ff17a0c2bbabccf96d33af4e08ba8061889b079d/bochs/bios"

# name  |  url  |  sha256
FILES=(
  "BIOS-bochs-legacy|$BASE/BIOS-bochs-legacy|d8848f08e6c832144d906d2c2175469a818a54062d42e8c7e60895b9e9aa930c"
  "VGABIOS-lgpl-latest.bin|$BASE/VGABIOS-lgpl/VGABIOS-lgpl-latest.bin|157ee2e631c429114e48a8051c029ee79f1dc5adae59dd18178a6c6673c7cf37"
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
  curl -sSfL --retry 3 -o "$tmp" "$url"
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

echo "BIOS + video BIOS ready."
