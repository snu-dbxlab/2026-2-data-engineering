"""
Test 3 -- step 3-3: the flush worker empties sealed pools.

The local check needs SQL, pg_stat_io and the student's functions: after an
insert-only load the sealed pool must be written by the worker (not the
backend) and turned READY, the pages must really be on disk, and a long run
of seals must go through without an error.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB"}


def fresh(s, table):
    s.create_extension()
    s.sql("DROP TABLE IF EXISTS %s" % table)
    s.sql("CREATE TABLE %s (id int PRIMARY KEY, pad text)" % table)
    s.sql("SELECT pg_stat_reset_shared('io')")


def insert(s, table, lo, hi):
    s.sql("INSERT INTO %s SELECT g, repeat('x', 80) FROM generate_series(%d, %d) g"
          % (table, lo, hi))


class TestAsyncFlush(unittest.TestCase):

    @weight(3)
    @number("3.1")
    def test_3_1_worker_empties_the_pool(self):
        """One seal: the worker writes the pool and turns it READY; pages survive a crash"""
        with harness.Server("snudbx", SETTINGS) as s:
            fresh(s, "t31")
            insert(s, "t31", 1, 150000)          # 2143 pages: pool 1 sealed at 1844
            self.assertTrue(s.wait_reset(), "the sealed pool never turned READY again")
            self.assertEqual(s.sql("SELECT snudbx_pool_state(1) || '|' || snudbx_pool_usage(1, 't31')"),
                             "READY|0", "pool 1 must be READY and empty once the worker is done")
            self.assertGreaterEqual(s.io_writes("background worker"), 1800,
                                    "pg_stat_io: the flush worker must have written the pool "
                                    "(call pgstat_report_stat(true) after each pool)")
            # A CHECKPOINT now makes recovery start after the INSERT, so the data
            # comes back only if the worker really wrote the pages before dropping them.
            s.sql("CHECKPOINT")
            s.crash()
        with harness.Server("snudbx", SETTINGS) as s:
            self.assertEqual(s.sql("SELECT count(*) || '|' || sum(id) FROM t31"),
                             "150000|11250075000",
                             "rows are missing after checkpoint + crash: pages were dropped "
                             "without being written")
            s.sql("DROP TABLE t31")

    @weight(2)
    @number("3.2")
    def test_3_2_many_seals(self):
        """900k rows (several seals): no error, no worker restart, no pool left SEALED, and the worker wrote the pools"""
        with harness.Server("snudbx", SETTINGS) as s:
            fresh(s, "t32")
            pid0 = s.worker_pid()
            insert(s, "t32", 1, 900000)
            self.assertTrue(s.wait_reset())
            gen = int(s.sql("SELECT snudbx_generation()"))
            self.assertGreater(gen, 3, "expected several seals")
            self.assertEqual(s.worker_pid(), pid0,
                             "the worker restarted during the load: it hit an ERROR")
            active = int(s.sql("SELECT snudbx_active_pool()"))
            self.assertEqual(s.sql("SELECT snudbx_pool_state(%d) || '|' || snudbx_pool_usage(%d, 't32')"
                                   % (3 - active, 3 - active)), "READY|0",
                             "every seal must end in a reset: the other data pool is READY and empty")
            self.assertGreaterEqual(s.io_writes("background worker"), 1500 * (gen - 1),
                                    "the flush worker, not the backend, must have written "
                                    "the sealed pools (about 1844 pages per reset)")
            self.assertEqual(s.sql("SELECT count(*) || '|' || sum(id) FROM t32"),
                             "900000|405000450000")
            s.sql("DROP TABLE t32")

    @weight(3)
    @number("3.3")
    def test_3_3_backend_writes_nothing_but_copies(self):
        """Insert-only: the backend writes at most the one page that straddles each seal; the worker wrote the pool"""
        with harness.Server("snudbx", SETTINGS) as s:
            fresh(s, "t33")
            insert(s, "t33", 1, 150000)
            self.assertTrue(s.wait_reset())
            gen = int(s.sql("SELECT snudbx_generation()"))
            self.assertLessEqual(s.settled_io_writes("client backend"), gen - 1,
                                 "with inserts only, the backend writes nothing itself; from step "
                                 "3-4 on it may copy out (and so write) the one page that straddles "
                                 "each seal")
            self.assertGreaterEqual(s.io_writes("background worker"), 1800)
            s.sql("DROP TABLE t33")
