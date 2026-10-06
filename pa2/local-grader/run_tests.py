#!/usr/bin/env python3
"""
Run the suite and write results.json.

With PA2_SUMMARY=1 (set by run_autograder on Gradescope) the per-test results
are kept for the instructor but hidden, and the student sees only one line per
step with its score.  The only failure named to the student is a build failure,
since without it a score of 0 has no explanation.
"""
import json
import os
import sys
import unittest

from gradescope_utils.autograder_utils.json_test_runner import JSONTestRunner

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Test number prefix -> step shown to students.
STEPS = [("1.", "Step 1-1"), ("2.", "Step 1-2"), ("3.", "Step 1-3")]
BUILD_GATE = ("0.1", "0.2")


def summarize(data):
    tests = data.get("tests", [])
    build_failed = any(t.get("number") in BUILD_GATE and t.get("status") == "failed"
                       for t in tests)
    summary = []
    if build_failed:
        summary.append({"name": "Build", "score": 0.0, "max_score": 0.0,
                        "status": "failed", "visibility": "visible",
                        "output": "Your tree does not build (default or snudbx "
                                  "configuration).  Every step scores 0."})
    total = 0.0
    for prefix, label in STEPS:
        mine = [t for t in tests if str(t.get("number", "")).startswith(prefix)]
        if not mine:
            continue
        mx = sum(t.get("max_score", 0.0) for t in mine)
        got = 0.0 if build_failed else sum(t.get("score", 0.0) for t in mine)
        total += got
        summary.append({"name": label, "score": got, "max_score": mx,
                        "visibility": "visible"})
    for t in tests:
        t["visibility"] = "hidden"
    data["tests"] = summary + tests
    data["score"] = total           # overrides the sum over (hidden) tests
    data["stdout_visibility"] = "hidden"
    return data


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.discover("tests")
    out = os.environ.get("PA2_RESULTS", "/autograder/results/results.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        JSONTestRunner(visibility="visible", stream=f).run(suite)
    if os.environ.get("PA2_SUMMARY") == "1":
        with open(out) as f:
            data = summarize(json.load(f))
        with open(out, "w") as f:
            json.dump(data, f, indent=2)
