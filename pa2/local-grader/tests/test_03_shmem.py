"""
Test 3 -- step 1-3: every pool has its own structures.

Graded through pg_shmem_allocations (a built-in view): each per-pool segment
name must appear once per pool.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

# Per-pool shared memory segments, named "<kind> <pool>".
KINDS = ["Buffer Blocks", "Buffer Descriptors", "Buffer IO Condition Variables",
         "Checkpoint BufferIds", "Shared Buffer Lookup Table", "Buffer Strategy Status"]

# 16MB per pool keeps eight pools affordable.
SMALL = {"shared_buffers": "16MB"}


def segments(s):
    """{name: size} for every allocation whose name ends in a pool number."""
    return {name: int(size) for name, size in s.rows(
        "SELECT name, size FROM pg_shmem_allocations WHERE name ~ ' [0-9]+$'")}


class TestPerPoolSegments(unittest.TestCase):

    @weight(2)
    @number("3.1")
    def test_one_segment_per_pool(self):
        """Each per-pool segment exists once per pool (buffer_pools = 3, 4, 8)"""
        for n in (3, 4, 8):
            with harness.Server("snudbx", dict(SMALL, buffer_pools=str(n))) as s:
                got = segments(s)
            for kind in KINDS:
                want = {"%s %d" % (kind, p) for p in range(n)}
                have = {k for k in got if k.rsplit(" ", 1)[0] == kind}
                self.assertEqual(have, want,
                                 'buffer_pools=%d: expected segments "%s 0" .. "%s %d"'
                                 % (n, kind, kind, n - 1))
