#!/usr/bin/env python3
"""
Run the suite and write results.json.

With PA3_SUMMARY=1 (set by run_autograder on Gradescope) the per-test results
are kept for the instructor but hidden, and the student sees only one line per
step with its score.  The only failures named to the student are a rejected
patch and a build failure, since without them a score of 0 has no explanation.

Test numbers are <step>.<item>: 1.x is Step 3-1, 2.x Step 3-2, and so on.  The
Gradescope bundle adds the instructor's tests to the same tests/ directory;
PA3_HIDDEN_TESTS may instead name a second directory to discover.
"""
import json
import os
import sys
import unittest

from gradescope_utils.autograder_utils.json_test_runner import JSONTestRunner

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness

# Test number prefix -> step shown to students.
STEPS = [("1.", "Step 3-1"), ("2.", "Step 3-2"), ("3.", "Step 3-3"), ("4.", "Step 3-4")]
# Display order of the per-step lines.
LABELS = ["Step 3-1", "Step 3-2", "Step 3-3", "Step 3-4"]
BUILD_GATE = ("0.1", "0.2")
PASS_FAIL = {}


def step_of(number):
    for prefix, label in STEPS:
        if str(number).startswith(prefix):
            return label
    return None


def number_key(number):
    """Sort key for "1.2"-style test numbers; unknown forms sort last."""
    try:
        return tuple(int(x) for x in str(number).split("."))
    except ValueError:
        return (99,)


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
    tests.sort(key=lambda t: number_key(t.get("number", "")))
    for t in tests:
        t["visibility"] = "hidden"
    data["tests"] = summary + tests
    data["score"] = total           # overrides the sum over (hidden) tests
    data["stdout_visibility"] = "hidden"
    return data


def load_suite():
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite([loader.discover("tests")])
    hidden = os.environ.get("PA3_HIDDEN_TESTS")
    if hidden and os.path.isdir(hidden):
        suite.addTests(loader.discover(hidden, top_level_dir=hidden))
    return suite


if __name__ == "__main__":
    out = os.environ.get("PA3_RESULTS", "/autograder/results/results.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        JSONTestRunner(visibility="visible", stream=f).run(load_suite())
    with open(out) as f:
        data = json.load(f)
    if os.environ.get("PA3_SUMMARY") == "1":
        data = summarize(data)
    else:
        data.get("tests", []).sort(key=lambda t: number_key(t.get("number", "")))
    with open(out, "w") as f:
        json.dump(data, f, indent=2)
