"""
Test 5 -- step 2-2: functions that sweep every pool.

The local check needs nothing but SQL: a small table written on the snudbx
server must survive a clean restart.  Its pages stay in data pool 1 until the
shutdown checkpoint, so they reach the disk only if BufferSync() covers pool 1.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

SETTINGS = {"shared_buffers": "16MB", "autovacuum": "off"}


class TestSweepLocal(unittest.TestCase):

    @weight(3)
    @number("5.1")
    def test_restart(self):
        """A small table survives a clean restart"""
        with harness.Server("snudbx", SETTINGS) as s:
            s.sql("DROP TABLE IF EXISTS r51")
            s.sql("CREATE TABLE r51 (id int, pad text)")
            s.sql("INSERT INTO r51 SELECT g, repeat('x', 100) FROM generate_series(1, 2000) g")
            before = s.sql("SELECT count(*), sum(id) FROM r51")
        with harness.Server("snudbx", SETTINGS) as s:
            self.assertEqual(s.sql("SELECT count(*), sum(id) FROM r51"), before,
                             "rows written before a clean shutdown are missing after restart")
