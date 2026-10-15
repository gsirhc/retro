#!/bin/sh
# Copies a machine's runtime files (the args) into DEST, minus source and tooling.
set -eu
dest=${1:-}
case $dest in
  /*) ;;
  *) echo "stage_web.sh: DEST must be an absolute path" >&2; exit 2 ;;
esac
shift
mkdir -p "$dest"
for f in "$@"; do cp -Rp "$f" "$dest/"; done
find "$dest" \( -name '*.py' -o -name '*.pyc' -o -name '*.sh' -o -name '*.md' -o -name '*.h' \
  -o -name '*.cpp' -o -name '__pycache__' -o -name '.DS_Store' \) -prune -exec rm -rf {} +
