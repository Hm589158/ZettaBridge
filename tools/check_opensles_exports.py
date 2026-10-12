#!/usr/bin/env python3
"""Checks that the guest libOpenSLES.so exports every symbol of the NDK stub library.

Usage: check_opensles_exports.py <llvm-nm> <ndk libOpenSLES.so> <guest libOpenSLES.so>

A guest library that imports one name we lack does not load at all. Symbol versions are
compared too: a version the NDK stub defines must also be ours, or references recorded
with that version would not bind.
"""
import subprocess
import sys


def exports(nm: str, path: str) -> dict:
    out = subprocess.run([nm, "-D", "--defined-only", path], check=True, capture_output=True, text=True).stdout
    symbols = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        name, _, version = fields[2].partition("@")
        symbols[name] = version.lstrip("@")
    return symbols


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    nm, ndk, ours = sys.argv[1:]
    want = exports(nm, ndk)
    have = exports(nm, ours)
    missing = sorted(set(want) - set(have))
    if missing:
        raise SystemExit(f"{ours} lacks NDK exports: {' '.join(missing)}")
    wrong = sorted(f"{name} ({have[name] or 'unversioned'}, NDK {version})"
                   for name, version in want.items() if version and have[name] != version)
    if wrong:
        raise SystemExit(f"{ours} symbol versions differ from the NDK stub: {', '.join(wrong)}")
    print(f"libOpenSLES exports OK ({len(want)} NDK symbols, {len(have)} ours)")


if __name__ == "__main__":
    main()
