"""
Test 1 -- the buffer_pools setting.

Graded entirely through pg_settings, a built-in catalog view: nothing here depends
on the student's own accessors.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness


class TestGuc(unittest.TestCase):

    @weight(2)
    @number("1.1")
    def test_guc_exists_with_right_shape(self):
        """buffer_pools has the required type, context, range and default"""
        with harness.Server("snudbx") as s:
            row = s.rows(
                "SELECT vartype, context, min_val, max_val, boot_val "
                "FROM pg_settings WHERE name = 'buffer_pools'")
        self.assertEqual(len(row), 1, "no setting named 'buffer_pools'")
        vartype, context, min_val, max_val, boot_val = row[0]
        self.assertEqual(vartype, "integer")
        self.assertEqual(context, "postmaster",
                         "buffer_pools must be PGC_POSTMASTER: it is consumed before "
                         "shared memory is sized, so it cannot be reloadable.")
        self.assertEqual(min_val, "3",
                         "buffer_pools counts all pools: one metadata pool plus at "
                         "least two data pools to alternate between.")
        self.assertEqual(max_val, "8", "MAX_BUFFER_POOLS must be 8")
        self.assertEqual(boot_val, "3")

    @weight(2)
    @number("1.5")
    def test_accepts_full_range(self):
        """Server starts and reports the setting for buffer_pools in 3..8"""
        for n in ("3", "4", "8"):
            with harness.Server("snudbx", {"buffer_pools": n}) as s:
                self.assertEqual(s.sql("SHOW buffer_pools"), n)
