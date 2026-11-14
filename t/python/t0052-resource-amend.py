#!/usr/bin/env python3
###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Unit tests for flux.resource.amend.load_amender()."""

import os
import tempfile
import unittest

import subflux  # noqa: F401 - configures PYTHONPATH for flux imports
from flux.resource import TreePool  # module, not the class
from flux.resource.amend import load_amender
from pycotap import TAPTestRunner


def write_file(contents):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as fp:
        fp.write(contents)
    return fp.name


class TestLoadAmender(unittest.TestCase):

    def test_module_function(self):
        self.assertIs(load_amender("os.path:basename"), os.path.basename)

    def test_module_function_missing_module(self):
        with self.assertRaisesRegex(RuntimeError, "amend-r.*no_such_module_xyz"):
            load_amender("no_such_module_xyz:fn")

    def test_module_function_missing_attribute(self):
        with self.assertRaisesRegex(RuntimeError, "amend-r.*no_such_function_xyz"):
            load_amender("os.path:no_such_function_xyz")

    def test_file(self):
        path = write_file("def amend(R, hwloc_xml=None):\n    return 42\n")
        try:
            self.assertEqual(load_amender(path)({}), 42)
        finally:
            os.unlink(path)

    def test_file_without_amend(self):
        path = write_file("def other(R, hwloc_xml=None):\n    return R\n")
        try:
            with self.assertRaisesRegex(RuntimeError, "no 'amend' callable"):
                load_amender(path)
        finally:
            os.unlink(path)

    def test_file_with_syntax_error(self):
        path = write_file("def amend(R\n")
        try:
            with self.assertRaisesRegex(RuntimeError, "failed to load"):
                load_amender(path)
        finally:
            os.unlink(path)

    def test_missing_file(self):
        with self.assertRaisesRegex(RuntimeError, "file not found"):
            load_amender("/no/such/path/amender.py")
        with self.assertRaisesRegex(RuntimeError, "file not found"):
            load_amender("amender.py")

    def test_plain_name(self):
        """A plain name selects flux.resource.<name>:amend"""
        self.assertIs(load_amender("TreePool"), TreePool.amend)

    def test_plain_name_not_found(self):
        with self.assertRaisesRegex(RuntimeError, "flux.resource.NoSuchPool"):
            load_amender("NoSuchPool")

    def test_file_in_cwd(self):
        """A bare name ending in .py is a file, not a plain name"""
        with tempfile.TemporaryDirectory() as tmpdir:
            cwd = os.getcwd()
            os.chdir(tmpdir)
            try:
                with open("TreePool.py", "w") as fp:
                    fp.write("def amend(R, hwloc_xml=None):\n    return 42\n")
                self.assertEqual(load_amender("TreePool.py")({}), 42)
            finally:
                os.chdir(cwd)


if __name__ == "__main__":
    unittest.main(testRunner=TAPTestRunner())
