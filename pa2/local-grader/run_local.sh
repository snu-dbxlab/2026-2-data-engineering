#!/usr/bin/env bash
# Local runner: grade a working tree directly, no patch, no Gradescope.
#   ./run_local.sh /path/to/postgres-18
set -uo pipefail
SRC="${1:?usage: run_local.sh /path/to/postgres-18}"
export PA2_SRC="$(cd "$SRC" && pwd)"
export PA2_WORK="${PA2_WORK:-/tmp/pa2-work}"
export PA2_RESULTS="${PA2_RESULTS:-$PA2_WORK/results.json}"
mkdir -p "$PA2_WORK"
cd "$(dirname "$0")"
python3 run_tests.py
python3 - "$PA2_RESULTS" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
total = earned = 0.0
for t in r.get("tests", []):
    got, mx = t.get("score", 0.0), t.get("max_score", 0.0)
    earned += got; total += mx
    print("%-6s %-62s %5.1f / %-5.1f" % (t.get("number", ""), t.get("name", "")[:62], got, mx))
    if got < mx and t.get("output"):
        print("       " + t["output"].strip().replace("\n", "\n       ")[:1500])
print("-" * 84)
print("%-69s %5.1f / %-5.1f" % ("TOTAL", earned, total))
PY
