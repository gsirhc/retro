#!/bin/sh
# Pinned, SHA-256-verified actionlint binary for make lint-actions.
set -eu
cd "$(dirname "$0")"

ver=1.7.12
dir=.tools/actionlint-$ver
bin=$dir/actionlint

if [ -x "$bin" ]; then
  echo "$bin"
  exit 0
fi

case "$(uname -s)/$(uname -m)" in
  Darwin/arm64)
    plat=darwin_arm64
    sha=aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f ;;
  Darwin/x86_64)
    plat=darwin_amd64
    sha=5b44c3bc2255115c9b69e30efc0fecdf498fdb63c5d58e17084fd5f16324c644 ;;
  Linux/x86_64)
    plat=linux_amd64
    sha=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 ;;
  Linux/aarch64|Linux/arm64)
    plat=linux_arm64
    sha=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6 ;;
  *)
    echo "no actionlint $ver build for $(uname -s)/$(uname -m)" >&2
    exit 1 ;;
esac

if command -v shasum >/dev/null 2>&1; then
  hash_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
  hash_of() { sha256sum "$1" | cut -d' ' -f1; }
else
  echo "shasum or sha256sum is required" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

url=https://github.com/rhysd/actionlint/releases/download/v$ver/actionlint_${ver}_$plat.tar.gz
curl -fsSL "$url" -o "$tmp/al.tar.gz"

got=$(hash_of "$tmp/al.tar.gz")
if [ "$got" != "$sha" ]; then
  echo "actionlint checksum mismatch for $url" >&2
  echo "  expected $sha" >&2
  echo "  got      $got" >&2
  exit 1
fi

mkdir -p "$dir"
tar -xzf "$tmp/al.tar.gz" -C "$tmp" actionlint
mv "$tmp/actionlint" "$bin"
echo "$bin"
