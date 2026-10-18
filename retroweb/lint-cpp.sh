#!/bin/sh
# cppcheck over retroweb source. Headers are C++ (a bare .h is C to cppcheck).
# One cppcheck run per machine, plus shared/.
set -eu
cd "$(dirname "$0")"

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "cppcheck is required: brew install cppcheck, or apt install cppcheck" >&2
  exit 1
fi

gtest=""
for d in /usr/include /opt/homebrew/include /usr/local/include; do
  if [ -f "$d/gtest/gtest.h" ]; then
    gtest=$d
    break
  fi
done
if [ -z "$gtest" ]; then
  hdr=$(find . -path '*/googletest-src/googletest/include/gtest/gtest.h' -print -quit || true)
  if [ -n "$hdr" ]; then
    gtest=$(dirname "$(dirname "$hdr")")
  fi
fi
if [ -z "$gtest" ]; then
  echo "gtest headers not found." >&2
  echo "apt install libgtest-dev, or brew install googletest," >&2
  echo "or build one native suite so cmake fetches googletest." >&2
  exit 1
fi

# Pass only the gtest dir: -I /usr/include makes cppcheck 2.13 syntax-error in stdlib.h.
gtest_inc=$gtest
gtest_tmp=""
case "$gtest" in
  /usr/include|/usr/local/include|/opt/homebrew/include)
    gtest_tmp=$(mktemp -d)
    ln -s "$gtest/gtest" "$gtest_tmp/gtest"
    gtest_inc=$gtest_tmp
    ;;
esac

files=$(mktemp)
trap 'rm -rf "$files" ${gtest_tmp:+"$gtest_tmp"}' EXIT

# One run per machine with only its own headers on the path, so a bare
# #include "ega.h" can't resolve to another machine's copy.
shared="-I shared -I shared/cpu -I shared/galaxian -I shared/atari -I shared/namco -I shared/pc -I shared/mw8080"
status=0
check() {
  dir=$1
  shift
  find "$dir" \( \
      -path '*/tests/build' -o -path '*/tests/build-cov' -o -path '*/tests/coverage-cpp' \
      -o -path '*/vendor' -o -path '*/web/shared' -o -path '*/node_modules' \
    \) -prune -o -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    ! -name 'hwtest_roms.h' -print | sort >"$files"
  # shellcheck disable=SC2086
  cppcheck --language=c++ --std=c++17 \
    --enable=warning,performance,portability \
    --error-exitcode=1 --inline-suppr --quiet \
    --max-configs=1 \
    --suppress=missingIncludeSystem \
    --suppress=toomanyconfigs \
    --suppress='*:*/gtest/*' \
    --suppress='*:/usr/include/*' \
    --suppress='*:/usr/include/c++/*' \
    --template='{file}:{line}: {severity}: {id}: {message}' \
    "$@" $shared -I "$gtest_inc" \
    --file-list="$files" || status=1
}

for m in altair8800 assembler6502 ibmpc-at pc486 pacman frogger galaxian scramble \
    galaga asteroids centipede missile invaders lunarrescue; do
  check "$m" -I "$m"
done
check shared
exit $status
