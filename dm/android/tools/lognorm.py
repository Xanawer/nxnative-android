#!/usr/bin/env python3
# Normalizes a host log for the Phase 1 parity test (docs/android/PLAN.md): the phone's log up to the first present must match a Mac run's.
# Drops what differs from run to run or between the two backends (addresses, paths, times, sizes, statistics and backend-tagged lines) and keeps the order of the HLE events.
# Usage: lognorm.py <log> [--until-present] > normalized.txt      Self-test: two Mac runs of the same game normalize to the same text.
import re, sys

BACKEND = re.compile(r"\[(nvn-mtl|nvn-vk|nvn-null|mtl|metal|vk)\]|pipeline (prewarm|archive)|translation (up to|detail)|thread policy:|packaged textures:|\[nvn\] frame \d+:| up to frame \d+:|audren: render time|\[longframe\]|\[slowframe\]|\[hitch\]")
SUBS = [
    (re.compile(r"(/[^\s:,()'\"]+)+"), "<path>"),
    (re.compile(r"0x[0-9a-fA-F]+"), "<hex>"),
    (re.compile(r"\b[0-9a-f]{12,}\b"), "<hash>"),
    (re.compile(r"\d+(\.\d+)?\s?(ns|us|µs|ms|s|min|MB|GB|KB|kB|B|%|Hz|MHz|GHz|M|K)\b"), "<n>"),
    (re.compile(r"\b\d+(\.\d+)?\b"), "<n>"),
]


def norm(lines, until_present):
    out = []
    for ln in lines:
        ln = ln.rstrip("\n")
        if until_present and re.search(r"first present|\[nvn\] frame 1\b|present #1\b", ln):
            break
        if BACKEND.search(ln):
            continue
        for rx, rep in SUBS:
            ln = rx.sub(rep, ln)
        out.append(ln)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    with open(args[0], errors="replace") as f:
        sys.stdout.write("\n".join(norm(f, "--until-present" in sys.argv)) + "\n")


if __name__ == "__main__":
    main()
