#!/usr/bin/env bash
# Fetch the real FreeDOS 1.3 multi-floppy *installer* set -- x86DSK01.img
# through x86DSK06.img from the same official "120m" (1.2MB, 5.25" HD)
# Floppy Edition distribution fetch-freedos.sh already pulls x86BOOT.img
# from. These five install disks are what build_freedos_hdd.cpp actually
# installs from: this machine's shipped hard disk image is produced by
# running the genuine, unmodified FreeDOS 1.3 installer end to end against
# the emulator (see IBM_PCAT_REVIEW.md §11), not by hand-crafting a
# filesystem, and the installer needs its real install floppies to do
# that. SHA-256 verified per file, same pattern as fetch-freedos.sh/
# fetch-basic.sh.
#
#   ./fetch-freedos-install-set.sh            # fetch what's missing/wrong
#   ./fetch-freedos-install-set.sh --force    # re-fetch everything

set -euo pipefail
cd "$(dirname "$0")"
FORCE="${1:-}"

URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip"

# name -> expected SHA-256, one entry per install floppy (x86BOOT.img
# itself stays fetch-freedos.sh's job).
NAMES=(x86DSK01.img x86DSK02.img x86DSK03.img x86DSK04.img x86DSK05.img x86DSK06.img)
SHAS=(
  64c4d020b380994bbe4a83a595e56948513e5b0c4cede81a0c1e476183b9debf
  5647b50055c865209dbf447534f274ad2835fd3b347420bf5be139918008008f
  a0b493fd8f7e656b7981c357f504c3fda85be4fc25ed8c46a4ecffee209bf5e1
  50521230e4ec10f29c8c55b4fcc89098583cc94a27326c4dc6aed4ab5d36b76f
  17afc3c49ebb7ac2ffc60f4ea9891fa5b4ba24c020d8c19e682a209c31efc884
  30c197348c0285b58eb10e44813e88d6a01a6f9fb462e5d6895d77c65fd27374
)

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }

need_fetch=0
for name in "${NAMES[@]}"; do
  [ -f "$name" ] || need_fetch=1
done
if [ -z "$FORCE" ] && [ "$need_fetch" = 0 ]; then
  ok=1
  for i in "${!NAMES[@]}"; do
    [ "$(sha "${NAMES[$i]}")" = "${SHAS[$i]}" ] || ok=0
  done
  if [ "$ok" = 1 ]; then
    echo "ok    ${NAMES[*]}"
    exit 0
  fi
fi

echo "fetch ${NAMES[*]} (via FD13-FloppyEdition.zip)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -sSfL --retry 3 -o "$tmp/fd.zip" "$URL"
for name in "${NAMES[@]}"; do
  unzip -o -q "$tmp/fd.zip" "120m/$name" -d "$tmp/x"
done

for i in "${!NAMES[@]}"; do
  name="${NAMES[$i]}"
  got="$(sha "$tmp/x/120m/$name")"
  if [ "$got" != "${SHAS[$i]}" ]; then
    echo "  SHA-256 mismatch for $name" >&2
    echo "  want ${SHAS[$i]}" >&2
    echo "  got  $got" >&2
    exit 1
  fi
done
for name in "${NAMES[@]}"; do
  mv "$tmp/x/120m/$name" "$name"
  chmod 644 "$name"
done
echo "FreeDOS 1.3 install floppy set ready."
