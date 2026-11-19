#!/usr/bin/env bash
# Local runner: grade a working tree directly, no patch, no Gradescope.
#   ./run_local.sh /path/to/postgres-18
set -uo pipefail
SRC="${1:?usage: run_local.sh /path/to/postgres-18}"
export PA3_SRC="$(cd "$SRC" && pwd)"
export PA3_WORK="${PA3_WORK:-/tmp/pa3-work}"
export PA3_RESULTS="${PA3_RESULTS:-$PA3_WORK/results.json}"
mkdir -p "$PA3_WORK"
cd "$(dirname "$0")"
python3 run_tests.py
python3 - "$PA3_RESULTS" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
def key(t):
    try:
        return tuple(int(x) for x in str(t.get("number", "")).split("."))
    except ValueError:
        return (99,)
total = earned = 0.0
for t in sorted(r.get("tests", []), key=key):
    got, mx = t.get("score", 0.0), t.get("max_score", 0.0)
    earned += got; total += mx
    print("%-6s %-62s %5.1f / %-5.1f" % (t.get("number", ""), t.get("name", "")[:62], got, mx))
    if got < mx and t.get("output"):
        print("       " + t["output"].strip().replace("\n", "\n       ")[:1500])
print("-" * 84)
print("%-69s %5.1f / %-5.1f" % ("TOTAL", earned, total))
PY
