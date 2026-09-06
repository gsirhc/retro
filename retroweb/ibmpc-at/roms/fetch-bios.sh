#!/usr/bin/env bash
# Fetch the freely-licensed firmware images this machine substitutes for
# IBM's still-copyrighted AT BIOS and EGA video BIOS (per CLAUDE.md's
# "Adding a new machine" rule) -- both from the Bochs Project, LGPL,
# pinned to a specific commit and verified by SHA-256. Run from anywhere;
# files land next to this script. Used by CI and locally.
#
#   ./fetch-bios.sh            # fetch what's missing / wrong
#   ./fetch-bios.sh --force    # re-fetch everything
#
# Why Bochs, not SeaBIOS: SeaBIOS needs a real i386-targeting cross
# compiler (16-bit real-mode codegen) to build from source, which isn't
# available in an ordinary interactive dev environment, and SeaBIOS itself
# publishes no prebuilt binaries. Bochs's own legacy BIOS (BIOS-bochs-
# legacy) is committed as a prebuilt binary directly in the Bochs source
# tree, and is specifically the plain-ISA/no-PCI/no-ACPI variant -- a
# better fit for a genuine AT-class machine than SeaBIOS's PCI-oriented
# default build would be anyway. Its video-BIOS counterpart
# (VGABIOS-lgpl-latest.bin) is likewise prebuilt and vendored the same way.
# See IBM_PCAT_REVIEW.md §6.
#
# Labelling (CLAUDE.md's substitution rule): BIOS-bochs-legacy is a
# compatible stand-in for IBM's genuine AT BIOS, not the genuine article.
# VGABIOS-lgpl-latest.bin is a full VGA-compatible video BIOS standing in
# for a genuine EGA card's onboard ROM -- it exposes more video modes than
# real EGA hardware has (VGA modes 10h-13h included); this machine's own
# EGA device (Phase 5) is what actually enforces the real EGA ceiling of
# 640x350x16, regardless of what modes this BIOS thinks it can offer.

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
