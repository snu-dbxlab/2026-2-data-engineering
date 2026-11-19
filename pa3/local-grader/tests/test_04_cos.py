"""
Test 4 -- step 3-4: copy-on-seal.

The local check runs a hot-update workload (appends plus updates of 100 hot
rows) with pgbench from the student's own build.  Without copy-on-seal the
updates dirty pages of the SEALED pool in place, the worker's reset fails on
them ("still dirty") and the worker restarts; with it the worker never
restarts, no update is lost, every seal ends in a reset, and the backend
writes at most a handful of pages per seal (the dirty originals it copies).
"""

import os
import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB"}
HOT_SQL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hot.sql")
TXNS = 1500


class TestCopyOnSeal(unittest.TestCase):

    @weight(5)
    @number("4.1")
    def test_4_1_hot_update_workload(self):
        """Appends + hot updates: no lost update, no worker restart, every seal reset, backend writes only a few pages"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.create_extension()
            s.sql("DROP TABLE IF EXISTS hot, append_t")
            s.sql("CREATE TABLE hot (id int PRIMARY KEY, v int)")
            s.sql("INSERT INTO hot SELECT g, 0 FROM generate_series(1, 100) g")
            s.sql("CREATE TABLE append_t (id int, pad text)")
            s.sql("SELECT pg_stat_reset_shared('io')")
            pid0 = s.worker_pid()
            ok, out = s.pgbench(HOT_SQL, TXNS)
            self.assertTrue(ok, "pgbench reported an error:\n%s" % out)
            self.assertTrue(s.wait_reset())
            self.assertEqual(s.sql("SELECT sum(v) FROM hot"), str(TXNS),
                             "a hot-row update was lost")
            gen = int(s.sql("SELECT snudbx_generation()"))
            self.assertGreater(gen, 1, "no pool was sealed")
            self.assertEqual(s.worker_pid(), pid0,
                             "the worker restarted during the workload: a page of the SEALED pool "
                             "was modified in place instead of being copied, so the worker's reset "
                             "found it dirty")
            active = int(s.sql("SELECT snudbx_active_pool()"))
            self.assertEqual(s.sql("SELECT snudbx_pool_state(%d)" % (3 - active)), "READY",
                             "every seal must end in a reset by the worker")
            self.assertGreaterEqual(s.io_writes("background worker"), 1500 * (gen - 1),
                                    "the flush worker, not the backend, must have written the "
                                    "sealed pools")
            self.assertLessEqual(s.settled_io_writes("client backend"), 10 * (gen - 1),
                                 "the backend may write a page only to copy it out clean: a few "
                                 "pages per seal, never the pool")
            s.sql("DROP TABLE hot, append_t")
