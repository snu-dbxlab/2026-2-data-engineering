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

import harness

# Test number prefix -> step shown to students.  A test belongs to the first
# prefix it matches, so the extra ("4.E") is kept apart from step 2-1.
STEPS = [("1.", "Step 1-1"), ("2.", "Step 1-2"), ("3.", "Step 1-3"),
         ("4.E", "Step 2-1 extra"), ("4.", "Step 2-1"),
         ("5.", "Step 2-2"), ("6.", "Step 2-3")]
# Display order of the per-step lines.
LABELS = ["Step 1-1", "Step 1-2", "Step 1-3", "Step 2-1", "Step 2-2",
          "Step 2-3", "Step 2-1 extra"]
BUILD_GATE = ("0.1", "0.2")
# Scored 0 here: the extra's points are given in manual grading, together
# with the NOTES.md explanation.  The line only says whether it passed.
PASS_FAIL = {"Step 2-1 extra":
             ("Passed. The extra points are given in manual grading, together "
              "with your explanation in NOTES.md.",
              "Not passed.")}


def step_of(number):
    for prefix, label in STEPS:
        if str(number).startswith(prefix):
            return label
    return None


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
    if harness._grader_error[0]:
        # The instructor probes include the headers and call the names the spec
        # fixes; if those differ, every probe-based test fails for one reason.
        summary.append({"name": "Instructor checks", "score": 0.0, "max_score": 0.0,
                        "status": "failed", "visibility": "visible",
                        "output": "The instructor's check module does not compile "
                                  "against your headers: a name or signature that "
                                  "the spec fixes is missing or different."})
    total = 0.0
    for label in LABELS:
        mine = [t for t in tests if step_of(t.get("number", "")) == label]
        if not mine:
            continue
        mx = sum(t.get("max_score", 0.0) for t in mine)
        got = 0.0 if build_failed else sum(t.get("score", 0.0) for t in mine)
        total += got
        line = {"name": label, "score": got, "max_score": mx,
                "visibility": "visible"}
        if label in PASS_FAIL:
            passed = not build_failed and all(t.get("status") == "passed"
                                              for t in mine)
            line["status"] = "passed" if passed else "failed"
            line["output"] = PASS_FAIL[label][0 if passed else 1]
        summary.append(line)
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
