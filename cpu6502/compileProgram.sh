#!/bin/sh

set -e

./vasm6502_oldstyle/vasm6502_oldstyle $1 -dotdir -Fbin
minipro -p AT28C256 -w a.out -u
