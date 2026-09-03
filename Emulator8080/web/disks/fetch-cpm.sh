#!/usr/bin/env bash
# Fetch the CP/M 2.2 system disk (freely redistributable since 2022 -- DRDOS
# Inc. / Bryan Sparks). The one the "CP/M Workstation" preset boots. Verified
# by SHA-256. The other disk images stay user-supplied.
#
#   ./fetch-cpm.sh            # fetch if missing / wrong
#   ./fetch-cpm.sh --force

set -euo pipefail
cd "$(dirname "$0")"

NAME="cpm63k.dsk"
URL="https://raw.githubusercontent.com/dhansel/Altair8800/8b0ac49448144f0afed1d5108559dc8507268a85/disks/DISK01.DSK"
WANT="730a806ae374c99d8c1ee1c4ab83b674ea7773050300c7d7aa4bfbd29752e4f2"

sha() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

if [ "${1:-}" = "" ] && [ -f "$NAME" ] && [ "$(sha "$NAME")" = "$WANT" ]; then
  echo "ok    $NAME"
  exit 0
fi

echo "fetch $NAME"
tmp="$(mktemp)"
curl -sSfL --retry 3 -o "$tmp" "$URL"
got="$(sha "$tmp")"
if [ "$got" != "$WANT" ]; then
  echo "  SHA-256 mismatch: want $WANT got $got" >&2
  rm -f "$tmp"; exit 1
fi
mv "$tmp" "$NAME"
chmod 644 "$NAME"
echo "CP/M 2.2 ready."
