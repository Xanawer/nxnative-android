#!/usr/bin/env python3
# Phase 1 parity check (docs/android/PLAN.md): a phone log against a Mac reference log, both up to the first present and normalized by lognorm.py (its norm, unchanged), compared with difflib.
# Passes (exit 0) only if neither log has a [RED], FATAL or CRASH line anywhere (raw lines, the whole log: a parity run is deterministic and clean throughout, and normalization drops
# backend-tagged lines while an identical red line in both logs would compare equal), and every line the phone log adds fully matches a pattern of the expected file's [+] section and every
# line it lacks one of its [-] section. Each log is cut at its first present record (PRESENT below), never at a line that only mentions a present. Exit 1 = a red line or a difference not
# expected, 2 = usage or a log that never presents (the prefix compared would not be the one the test means).
# Usage: parity.py <reference log> <phone log> [--expected <file>]    The expected file: "[+]" / "[-]" start a section, then one Python regex per line (matched against the whole normalized
# line); blank lines and lines starting with # are skipped. No --expected (or an empty file) = the normalized logs must be identical
import difflib, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lognorm import norm  # noqa: E402

RED = re.compile(r"\[RED\]|FATAL|CRASH")
# What the host prints once a present has happened: hle_os.c dm_guest_frame_tick's first call under DM_DETCLOCK (the end of the first nvnQueuePresentTexture, hle_nvn.c dump_frame), and
# dump_frame's line for frame 1 (the second present; frames 1-3 are always listed unless DM_NVN_FRAMES is lowered). lognorm's --until-present also cuts at any line mentioning "first present"
PRESENT = re.compile(r"\[dm\] DM_DETCLOCK: from the first present on, the guest clock advances by presents, step \d+ ns|\[dm\] \[nvn\] frame 1: \d+ functions \d+ calls( \|.*)?")


def read(path):
    with open(path, errors="replace") as f:
        return [ln.rstrip("\n") for ln in f]


def reds(path, lines):
    return [f"{path}:{n}: {ln}" for n, ln in enumerate(lines, 1) if RED.search(ln)]


def cut(path, lines):
    for n, ln in enumerate(lines):
        if PRESENT.fullmatch(ln):
            return norm(lines[:n], False)
    sys.exit(f"parity: {path} never reaches the first present (no DM_DETCLOCK first-present line, no [nvn] frame 1 line): nothing to compare")


def expected(path):
    pats, sec = {"+": [], "-": []}, None
    if not path:
        return pats
    with open(path) as f:
        for n, ln in enumerate(f, 1):
            ln = ln.rstrip("\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            if ln.strip() in ("[+]", "[-]"):
                sec = ln.strip()[1]
                continue
            if sec is None:
                sys.exit(f"parity: {path}:{n}: a pattern before any [+] / [-] section")
            pats[sec].append((re.compile(ln), f"{path}:{n}"))
    return pats


def main():
    a = sys.argv[1:]
    exp = None
    if "--expected" in a:
        i = a.index("--expected")
        if i + 1 >= len(a):
            sys.exit("usage: parity.py <reference log> <phone log> [--expected <file>]")
        exp = a[i + 1]
        del a[i:i + 2]
    if len(a) != 2:
        sys.exit("usage: parity.py <reference log> <phone log> [--expected <file>]")
    raw = [(p, read(p)) for p in a]
    red = [r for p, lines in raw for r in reds(p, lines)]
    if red:
        print(f"parity: {len(red)} [RED] / FATAL / CRASH line{'s' * (len(red) != 1)} (raw, whole logs)")
        for r in red:
            print("  red " + r)
        print("parity: FAIL")
        return 1
    (ref, got), pats = [cut(p, lines) for p, lines in raw], expected(exp)
    bad, seen = [], {"+": 0, "-": 0}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ref, got, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        for sign, lines in (("-", ref[i1:i2]), ("+", got[j1:j2])):
            for ln in lines:
                seen[sign] += 1
                if not next((w for rx, w in pats[sign] if rx.fullmatch(ln)), None):
                    bad.append(f"{sign} {ln}")
    print(f"parity: {len(ref)} reference lines, {len(got)} phone lines (normalized, up to the first present); {seen['+']} added and {seen['-']} removed, {len(bad)} not expected")
    for b in bad:
        print("  unexpected " + b)
    print("parity: " + ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            sys.exit(2)
        raise
