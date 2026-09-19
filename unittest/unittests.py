##
# @file   unittest.py
# @author Yibo Lin
# @date   Mar 2019
#

import os
import sys
import unittest

loader = unittest.TestLoader()
start_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ops")
print("search unittests in %s" % (start_dir))
suite = loader.discover(start_dir, pattern='*_unittest.py')

runner = unittest.TextTestRunner()
result = runner.run(suite)
# A launcher that exits zero on a failed suite is a false green; report the
# real verdict to whatever runs it.
sys.exit(0 if result.wasSuccessful() else 1)
