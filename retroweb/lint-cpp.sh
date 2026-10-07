#!/bin/sh
# cppcheck over retroweb source. Headers are C++ (a bare .h is C to cppcheck).
# Each machine is its own program, so a repeated Machine class is not an ODR bug.
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
find . \( \
    -path ./_site -o -path ./_site.new -o -path ./node_modules \
    -o -path '*/tests/build' -o -path '*/tests/build-cov' -o -path '*/tests/coverage-cpp' \
    -o -path '*/vendor' -o -path '*/web/shared' \
  \) -prune -o -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
  ! -name 'hwtest_roms.h' -print | sort >"$files"

cppcheck --language=c++ --std=c++17 \
  --enable=warning,performance,portability \
  --error-exitcode=1 --inline-suppr --quiet \
  --max-configs=1 \
  --suppress=missingIncludeSystem \
  --suppress=ctuOneDefinitionRuleViolation \
  --suppress=toomanyconfigs \
  --suppress='*:*/gtest/*' \
  --suppress='*:/usr/include/*' \
  --suppress='*:/usr/include/c++/*' \
  --template='{file}:{line}: {severity}: {id}: {message}' \
  -I altair8800 -I assembler6502 -I ibmpc-at -I pacman -I frogger \
  -I galaxian -I scramble -I asteroids \
  -I shared/cpu -I shared/galaxian -I shared/atari -I shared \
  -I "$gtest_inc" \
  --file-list="$files"
