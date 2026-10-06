"""
Test 2 -- step 1-2: the pool control block and the snudbx_* functions.

The control block's layout is the student's, so it is graded from outside:
pg_shmem_allocations (a built-in view) shows that it exists and that it is
sized by the number of pools, and the three UDFs are checked against the
buffer_pools setting the server was started with.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness

CTL = "SELECT size FROM pg_shmem_allocations WHERE name = 'Buffer Pool Control'"


class TestPoolControl(unittest.TestCase):

    @weight(2)
    @number("2.1")
    def test_control_block_allocated(self):
        """Shared memory has one segment named "Buffer Pool Control\""""
        with harness.Server("snudbx") as s:
            rows = s.rows(CTL)
            self.assertEqual(len(rows), 1,
                             'no shared memory segment named "Buffer Pool Control"; '
                             "allocate it with ShmemInitStruct() in BufferManagerShmemInit().")
            self.assertGreater(int(rows[0][0]), 0)

    @weight(2)
    @number("2.4")
    def test_extension_installs(self):
        """CREATE EXTENSION snudbx works and defines the three functions"""
        with harness.Server("snudbx") as s:
            s.create_extension()
            sigs = s.rows(
                "SELECT p.proname, p.pronargs, p.prorettype::regtype "
                "FROM pg_proc p JOIN pg_depend d ON d.objid = p.oid "
                "JOIN pg_extension e ON e.oid = d.refobjid "
                "WHERE e.extname = 'snudbx' AND d.deptype = 'e' ORDER BY 1")
            self.assertEqual(sigs, [["snudbx_active_pool", "0", "integer"],
                                    ["snudbx_generation", "0", "bigint"],
                                    ["snudbx_pool_count", "0", "integer"]])
