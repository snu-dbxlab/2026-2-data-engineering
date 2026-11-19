"""
Test 1 -- step 3-1: the flush worker exists and is managed by the postmaster.

Everything here comes from the OS and from pg_stat_activity, a built-in view:
nothing depends on the student's extension.
"""

import os
import signal
import subprocess
import time
import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

NAME = harness.WORKER_TYPE


class TestWorker(unittest.TestCase):

    @weight(2)
    @number("1.1")
    def test_1_1_worker_is_running(self):
        """One 'snudbx flush worker' process, seen in ps and in pg_stat_activity"""
        with harness.Server("snudbx") as s:
            pids = s.rows("SELECT pid FROM pg_stat_activity WHERE backend_type = '%s'" % NAME)
            self.assertEqual(len(pids), 1,
                             "pg_stat_activity must show exactly one row with backend_type "
                             "'%s' (bgw_type; the worker must call "
                             "BackgroundWorkerInitializeConnection())" % NAME)
            ps = subprocess.run(["ps", "-o", "args=", "-p", pids[0][0]],
                                stdout=subprocess.PIPE, text=True).stdout
            self.assertIn(NAME, ps,
                          "the process title must contain '%s' (bgw_name)" % NAME)

    @weight(2)
    @number("1.2")
    def test_1_2_restarts_after_sigterm(self):
        """After SIGTERM a new worker is started within 3 s, and nothing else restarts"""
        with harness.Server("snudbx") as s:
            old = s.worker_pid()
            self.assertIsNotNone(old, "no flush worker in pg_stat_activity")
            os.kill(old, signal.SIGTERM)
            new = None
            deadline = time.time() + 3
            while time.time() < deadline:
                new = s.worker_pid()
                if new and new != old:
                    break
                time.sleep(0.1)
            self.assertTrue(new and new != old,
                            "no new worker within 3 s of SIGTERM (bgw_restart_time)")
            self.assertNotIn("terminating any other active server processes", s.logtext(),
                             "the worker's exit took the whole server down: it must leave "
                             "with exit code 1 (die), not crash")
            self.assertEqual(s.sql("SELECT 1"), "1")
