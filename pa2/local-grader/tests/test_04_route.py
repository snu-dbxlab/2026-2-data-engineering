"""
Test 4 -- step 2-1: pages are routed to the metadata pool or the data pool.

The local check looks only through the student's snudbx_pool_usage(): after
reading a table made beforehand, its heap is in data pool 1 and its index in
metadata pool 0.  The table is created with the default build, so the snudbx
server reads it from disk rather than finding it where creation left it.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB", "autovacuum": "off"}

# A small heap (fits in one pool) with an index and out-of-line toast values.
FIXTURE = [
    "DROP TABLE IF EXISTS r41 CASCADE",
    "CREATE TABLE r41 (id int PRIMARY KEY, pad text, big text)",
    "ALTER TABLE r41 ALTER big SET STORAGE EXTERNAL",
    "INSERT INTO r41 SELECT g, repeat('x', 100), "
    "CASE WHEN g % 100 = 0 THEN repeat('z', 3000) END "
    "FROM generate_series(1, 20000) g",
    "VACUUM ANALYZE r41",
]

# Reads the index and the toast table through an index scan, then every heap
# block through a sequential scan (an index-only scan would skip the heap).
READ = ("SET enable_seqscan = off; SET enable_bitmapscan = off; "
        "SET enable_indexonlyscan = off; "
        "SELECT count(*), sum(length(big)) FROM r41 WHERE id > 0; "
        "SET enable_seqscan = on; SET enable_indexscan = off; "
        "SELECT count(*) FROM r41")


def prepare(statements):
    """Run statements on the default server, which writes everything to disk."""
    with harness.Server("default", SETTINGS) as s:
        for q in statements:
            s.sql(q)


class TestRouteLocal(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        prepare(FIXTURE)

    @weight(3)
    @number("4.6")
    def test_pool_usage(self):
        """snudbx_pool_usage() sees the heap in pool 1 and the index in pool 0"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.create_extension()
            sig = s.rows("SELECT pronargs, pronargdefaults, prorettype::regtype, proisstrict "
                         "FROM pg_proc WHERE proname = 'snudbx_pool_usage'")
            self.assertEqual(sig, [["2", "1", "integer", "f"]],
                             "snudbx_pool_usage(pool_id integer, rel regclass DEFAULT NULL) "
                             "returns integer and is not STRICT")
            s.sql(READ)
            heap_blocks = int(s.sql("SELECT pg_relation_size('r41') / 8192"))
            use = lambda p, rel: int(s.sql("SELECT snudbx_pool_usage(%d, %s)" % (p, rel)))
            self.assertEqual(use(1, "'r41'"), heap_blocks,
                             "every heap block of a table that was read should be in pool 1")
            self.assertGreater(use(0, "'r41_pkey'"), 0, "the index should be in pool 0")
            self.assertEqual(use(1, "'r41_pkey'"), 0, "no index block should be in pool 1")
            self.assertEqual(use(2, "'r41'"), 0, "pool 2 is not active yet")
            self.assertGreaterEqual(int(s.sql("SELECT snudbx_pool_usage(1)")), heap_blocks,
                                    "snudbx_pool_usage(1) without rel counts every page")
