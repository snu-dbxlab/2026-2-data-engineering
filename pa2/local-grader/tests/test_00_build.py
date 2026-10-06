"""
Build gate -- both configurations must compile.  No points of its own: if either
build fails, every step scores 0, and that is the one failure reported to the
student by name.

The default build is the one that matters pedagogically: it is the only thing
that enforces the #ifdef SNUDBX discipline.  A student who edits a shared
header without guarding it breaks the default PostgreSQL build, and nothing
else in the suite would notice.
"""

import unittest
from gradescope_utils.autograder_utils.decorators import weight, number

import harness


class TestBuild(unittest.TestCase):

    @weight(0)
    @number("0.1")
    def test_default_builds(self):
        """default build compiles (proves shared-file edits are #ifdef'd)"""
        try:
            harness.build("default")
        except harness.BuildError as e:
            self.fail("The default configuration failed to build.  Every change to a\n"
                      "shared file must be inside #ifdef SNUDBX.\n\n%s" % e)

    @weight(0)
    @number("0.2")
    def test_snudbx_builds(self):
        """SNUDBX build compiles"""
        try:
            harness.build("snudbx")
        except harness.BuildError as e:
            self.fail("The snudbx configuration failed to build.\n\n%s" % e)
