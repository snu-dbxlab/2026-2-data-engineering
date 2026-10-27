"""
Test 6 -- step 2-3: no-evict data pools, sealed when full.

The local check needs nothing but SQL and snudbx_generation(): a table larger
than one pool, updated and scanned, must come back unchanged after a restart,
and at least one pool must have been sealed on the way.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB"}

# About 4500 heap blocks: more than two 2048-frame pools.
BIG = [
    "DROP TABLE IF EXISTS t61",
    "CREATE TABLE t61 AS SELECT g AS id, repeat('x', 80) AS pad "
    "FROM generate_series(1, 300000) g",
    "UPDATE t61 SET pad = repeat('y', 80) WHERE id % 3 = 0",
]
CHECKSUM = ("SELECT count(*), sum(id), sum(length(pad)), "
            "count(*) FILTER (WHERE pad LIKE 'y%') FROM t61")
EXPECTED = "300000|45000150000|24000000|100000"


class TestSealLocal(unittest.TestCase):

    @weight(5)
    @number("6.1")
    def test_big_table(self):
        """A table larger than one pool survives updates and a restart (buffer_pools = 3, 4)"""
        for n in (3, 4):
            cfg = dict(SETTINGS, buffer_pools=str(n))
            with harness.Server("snudbx", cfg) as s:
                s.create_extension()
                for q in BIG:
                    s.sql(q)
                before = s.sql(CHECKSUM)
                gen = int(s.sql("SELECT snudbx_generation()"))
            with harness.Server("snudbx", cfg) as s:
                after = s.sql(CHECKSUM)
                s.sql("DROP TABLE t61")
            self.assertEqual(before, EXPECTED, "buffer_pools=%d: wrong contents" % n)
            self.assertEqual(after, before,
                             "buffer_pools=%d: contents changed across a restart" % n)
            self.assertGreater(gen, 1, "buffer_pools=%d: no pool was sealed" % n)
