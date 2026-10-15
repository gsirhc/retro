"""Rewrites a PC app.js so the ?test=1&fast=1 CPU multiplier is a constant 1."""
import sys

EXPR = 'testParams.get("test") === "1" && testParams.get("fast") === "1" ? 20 : 1;'


def main():
    check = sys.argv[1] == "--check"
    paths = sys.argv[2:] if check else sys.argv[1:]
    for path in paths:
        src = open(path).read()
        n = src.count(EXPR)
        if n != 1:
            sys.exit(f"strip_fast_flag: {path}: expected one test CPU multiplier, found {n}")
        out = src.replace(EXPR, "1;")
        if 'get("fast")' in out:
            sys.exit(f"strip_fast_flag: {path}: another fast path remains")
        if not check:
            open(path, "w").write(out)


main()
