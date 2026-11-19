"""
Test 2 -- step 3-2: the state shared by the backend and the worker.

The local check uses the student's observation functions and the numbers that
PA2's step 2-3 fixed: with the worker in place, the backend's seals must still
give the same pools, generations and usage counts.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB"}

# Step 3-2's functions: name, number of arguments, return type.
FUNCS = [["snudbx_flush_worker_pid", "0", "integer"],
         ["snudbx_pool_state", "1", "text"],
         ["snudbx_sealed_pool", "0", "integer"]]

STATE = ("SELECT snudbx_active_pool() || '|' || snudbx_generation() || '|' || "
         "snudbx_pool_usage(1, 't23') || '|' || snudbx_pool_usage(2, 't23')")


def insert(s, lo, hi):
    s.sql("INSERT INTO t23 SELECT g, repeat('x', 80) FROM generate_series(%d, %d) g" % (lo, hi))


class TestShared(unittest.TestCase):

    @weight(2)
    @number("2.1")
    def test_2_1_functions(self):
        """contrib/snudbx defines step 3-2's three functions with the right signatures"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.create_extension()
            sigs = s.rows(
                "SELECT p.proname, p.pronargs, p.prorettype::regtype "
                "FROM pg_proc p JOIN pg_depend d ON d.objid = p.oid "
                "JOIN pg_extension e ON e.oid = d.refobjid "
                "WHERE e.extname = 'snudbx' AND d.deptype = 'e' "
                "AND p.proname IN (%s) ORDER BY 1"
                % ", ".join("'%s'" % f[0] for f in FUNCS))
            self.assertEqual(sigs, FUNCS)

    @weight(2)
    @number("2.2")
    def test_2_2_initial_state(self):
        """Fresh server: worker pid published, no SEALED pool, pools ACTIVE/ACTIVE/READY"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.create_extension()
            self.assertEqual(s.sql("SELECT snudbx_flush_worker_pid()"), str(s.worker_pid()),
                             "snudbx_flush_worker_pid() must be the worker's pid "
                             "(flush_worker_proc published at startup)")
            self.assertEqual(
                s.sql("SELECT snudbx_sealed_pool() || '|' || snudbx_pool_state(0) || '|' || "
                      "snudbx_pool_state(1) || '|' || snudbx_pool_state(2)"),
                "-1|ACTIVE|ACTIVE|READY")

    @weight(3)
    @number("2.3")
    def test_2_3_pa2_numbers_still_hold(self):
        """PA2's step 2-3 scenario: 715 and 1429 frames, then a seal; pool 1 emptied, data intact"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.create_extension()
            s.sql("DROP TABLE IF EXISTS t23")
            s.sql("CREATE TABLE t23 (id int, pad text)")
            insert(s, 1, 50000)
            self.assertEqual(s.sql(STATE), "1|1|715|0")
            insert(s, 50001, 100000)
            self.assertEqual(s.sql(STATE), "1|1|1429|0")
            insert(s, 100001, 150000)
            self.assertTrue(s.wait_reset(), "pool 1 was sealed but never turned READY again")
            active, gen, p1, p2 = s.sql(STATE).split("|")
            self.assertEqual((active, gen, p1), ("2", "2", "0"),
                             "after the seal: active pool 2, generation 2, pool 1 emptied")
            self.assertIn(int(p2), (299, 300, 301), "about 300 pages of t23 in pool 2")
            self.assertEqual(s.sql("SELECT snudbx_pool_state(1) || '|' || snudbx_sealed_pool()"),
                             "READY|-1")
            self.assertEqual(s.sql("SELECT count(*) || '|' || sum(id) FROM t23"),
                             "150000|11250075000")
            s.sql("DROP TABLE t23")
