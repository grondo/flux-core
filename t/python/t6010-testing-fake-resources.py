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
#
# flux.testing.fake_resources tests
#
# Pure-Python tests for saturation_count() and build_R_encode_args(),
# plus broker-requiring tests for install() (full four-step sequence)
# and reload_scheduler() against a real Flux instance.
#

import contextlib
import io
import os
import unittest

import flux
import flux.resource
from flux.modprobe import ModuleList
from flux.testing.fake_resources import (
    InjectFakeResources,
    build_R_encode_args,
    reload_scheduler,
    saturation_count,
)
from subflux import rerun_under_flux


def __flux_size():
    return 1


class _BaseTestCase(unittest.TestCase):
    """Print the running test name to fd 2 at the start of each test.

    pycotap's TAPTestRunner captures sys.stderr per test and emits the
    contents as TAP diagnostics *after* the result line, which defeats
    the hang-debugging purpose: a test that hangs never reaches the
    point where the buffer is flushed, so its "Starting" line never
    appears. Writing directly to fd 2 with os.write bypasses pycotap's
    sys.stderr capture and shows the test name before the work starts.
    """

    def setUp(self):
        name = self.id().rpartition(".")[2]
        os.write(2, f"# Starting: {name}\n".encode())


# Pure-Python tests below — no broker required.


class TestSaturationCount(unittest.TestCase):
    """saturation_count() is pure arithmetic on the flat shape."""

    def test_cores_only(self):
        """1-core slots saturate at nodes * cores_per_node"""
        fr = InjectFakeResources(nodes=10, cores_per_node=64)
        self.assertEqual(fr.saturation_count(slot_cores=1), 640)

    def test_cores_floor(self):
        """slot_cores divides cores_per_node with floor division"""
        # 64 / 5 = 12 per node, 120 across 10 nodes (4 cores wasted).
        fr = InjectFakeResources(nodes=10, cores_per_node=64)
        self.assertEqual(fr.saturation_count(slot_cores=5), 120)

    def test_gpu_limited(self):
        """GPU constraint dominates when more limiting than cores"""
        # 64 cores would allow 64 single-core jobs per node; the
        # 1-GPU-per-slot constraint caps it at 8 per node.
        fr = InjectFakeResources(
            nodes=10,
            cores_per_node=64,
            gpus_per_node=8,
        )
        self.assertEqual(
            fr.saturation_count(slot_cores=1, slot_gpus=1),
            80,
        )

    def test_invalid_slot(self):
        """slot_cores < 1 and slot_gpus < 1 raises ValueError"""
        fr = InjectFakeResources(nodes=10, cores_per_node=64)
        with self.assertRaises(ValueError):
            fr.saturation_count(slot_cores=0, slot_gpus=0)


class TestSaturationCountFunction(unittest.TestCase):
    """saturation_count() as a free function — used by code that
    has a resource shape without a FakeResources object (e.g.
    flux-schedbench's ``--exec`` path, which gathers the shape
    from a real broker query)."""

    def test_matches_method_form(self):
        """The free function and FakeResources.saturation_count
        produce identical results for the same shape and slot.
        The method is a thin wrapper around the function, so
        any divergence is a bug."""
        fr = InjectFakeResources(
            nodes=10,
            cores_per_node=64,
            gpus_per_node=8,
        )
        for slot_cores, slot_gpus in [
            (1, 0),
            (5, 0),
            (1, 1),
            (2, 1),
            (4, 2),
        ]:
            with self.subTest(
                slot_cores=slot_cores,
                slot_gpus=slot_gpus,
            ):
                method = fr.saturation_count(
                    slot_cores=slot_cores,
                    slot_gpus=slot_gpus,
                )
                func = saturation_count(
                    fr.nodes,
                    fr.cores_per_node,
                    fr.gpus_per_node,
                    slot_cores=slot_cores,
                    slot_gpus=slot_gpus,
                )
                self.assertEqual(method, func)

    def test_no_fake_resources_object_needed(self):
        """saturation_count operates on plain ints; no
        FakeResources instance required. Verifies the function
        is callable from code paths that obtain resource shape
        from elsewhere (real broker query, hand-constructed
        dict)."""
        self.assertEqual(
            saturation_count(10, 64, 8, slot_cores=1, slot_gpus=1),
            80,
        )
        self.assertEqual(
            saturation_count(4, 4, 0, slot_cores=1),
            16,
        )

    def test_invalid_slot_raises(self):
        """slot_cores=0 and slot_gpus=0 is rejected in the
        free function too (not just the method)."""
        with self.assertRaises(ValueError):
            saturation_count(10, 64, 8, slot_cores=0, slot_gpus=0)


class TestREncodeArgs(unittest.TestCase):
    """build_R_encode_args() constructs the right argv shape."""

    def test_minimal(self):
        """Default shape: ranks 0-N, hostlist prefix[0-N], single core"""
        fr = InjectFakeResources(nodes=10)
        cmd = build_R_encode_args(fr)
        self.assertEqual(cmd[:3], ["flux", "R", "encode"])
        self.assertIn("0-9", cmd)  # rank idset
        self.assertIn("fake[0-9]", cmd)  # hostlist
        self.assertNotIn("-g", cmd)  # no gpus

    def test_with_gpus(self):
        """gpus_per_node > 0 adds the -g flag with a 0-based range"""
        fr = InjectFakeResources(
            nodes=4,
            cores_per_node=8,
            gpus_per_node=2,
        )
        cmd = build_R_encode_args(fr)
        self.assertIn("0-3", cmd)  # ranks
        self.assertIn("0-7", cmd)  # cores per node
        self.assertIn("-g", cmd)
        self.assertIn("0-1", cmd)  # gpus per node

    def test_with_one_gpu_per_node(self):
        """gpus_per_node=1 emits -g '0', not '0-0'.

        Regression test: a degenerate single-element idset
        encoded as ``"0-0"`` fails to parse when ``flux R encode``
        reads the -g argument, silently producing R without
        GPUs. IDset emits ``"0"`` for a one-element set, which
        parses cleanly. The pre-fix code wrote
        ``f"0-{n-1}"`` directly, producing ``"0-0"`` for the
        one-GPU case.
        """
        fr = InjectFakeResources(
            nodes=4,
            cores_per_node=2,
            gpus_per_node=1,
        )
        cmd = build_R_encode_args(fr)
        self.assertIn("-g", cmd)
        g_idx = cmd.index("-g")
        self.assertEqual(cmd[g_idx + 1], "0")

    def test_with_one_core_per_node(self):
        """cores_per_node=1 emits -c '0', not '0-0'.

        Same degenerate-range bug as gpus_per_node=1: a single
        core would have produced ``-c 0-0`` which fails to
        parse as an idset.
        """
        fr = InjectFakeResources(
            nodes=4,
            cores_per_node=1,
        )
        cmd = build_R_encode_args(fr)
        c_idx = cmd.index("-c")
        self.assertEqual(cmd[c_idx + 1], "0")

    def test_with_one_node(self):
        """nodes=1 emits -r '0', not '0-0'.

        Same degenerate-range bug as the per-node-resource
        cases: a single-rank deployment would have produced
        ``-r 0-0`` which fails to parse as an idset.
        """
        fr = InjectFakeResources(nodes=1, cores_per_node=4)
        cmd = build_R_encode_args(fr)
        r_idx = cmd.index("-r")
        self.assertEqual(cmd[r_idx + 1], "0")

    def test_custom_host_prefix(self):
        """host_prefix overrides the default 'fake'"""
        fr = InjectFakeResources(nodes=4, host_prefix="node")
        cmd = build_R_encode_args(fr)
        self.assertIn("node[0-3]", cmd)
        self.assertNotIn("fake[0-3]", cmd)


# Broker-requiring tests below.


class TestInjectFakeResources(_BaseTestCase):
    """install() runs the four-step sequence against a real broker.

    Each test mutates broker state (resource shape and scheduler
    module). Test ordering within this class doesn't matter — every
    test re-runs install() and asserts the post-install state. Tests
    in other classes (TestReloadScheduler) tolerate whatever fake R
    was installed last; both classes leave sched-simple loaded.
    """

    def test_install_cores_only(self):
        """install() with no GPUs: nodes/cores match, sched loaded"""
        h = flux.Flux()
        InjectFakeResources(
            nodes=4,
            cores_per_node=4,
            gpus_per_node=0,
        ).install(h)
        rset = flux.resource.resource_list(h).get().all
        self.assertEqual(rset.nnodes, 4)
        self.assertEqual(rset.ncores, 16)
        self.assertEqual(
            ModuleList(h).lookup("sched"),
            "sched-simple",
        )

    def test_install_with_gpus(self):
        """install() with GPUs: ngpus matches nodes * gpus_per_node"""
        h = flux.Flux()
        InjectFakeResources(
            nodes=2,
            cores_per_node=8,
            gpus_per_node=2,
        ).install(h)
        rset = flux.resource.resource_list(h).get().all
        self.assertEqual(rset.nnodes, 2)
        self.assertEqual(rset.ncores, 16)
        self.assertEqual(rset.ngpus, 4)

    def _assert_gpu_job_allocates(self, gpus_per_node):
        """Install fake R with the given GPU count, submit one
        1-GPU job via BulkRun, and assert it reaches 'alloc'.

        ``resource_list`` counting GPUs is necessary but not
        sufficient: sched-simple must also see them and use them
        for matching. This crosses both layers.
        """
        from flux.job import JobspecV1
        from flux.testing.bulkrun import BulkRun

        h = flux.Flux()
        InjectFakeResources(
            nodes=4,
            cores_per_node=2,
            gpus_per_node=gpus_per_node,
        ).install(h)

        spec = JobspecV1.from_command(
            ["true"],
            num_tasks=1,
            cores_per_task=1,
            gpus_per_task=1,
        )
        spec.attributes.setdefault("system", {}).setdefault(
            "exec",
            {},
        )[
            "test"
        ] = {"run_duration": "0.001s"}
        spec.duration = 5

        bulk = BulkRun(
            h,
            events_of_interest=(
                "alloc",
                "start",
                "exception",
                "clean",
            ),
        )
        bulk.push_jobs(spec.dumps(), 1)
        result = bulk.run()

        # Build a useful failure message that surfaces the
        # broker's actual complaint, since this is exactly the
        # path where "unsatisfiable resource request" hides bugs.
        n_alloc = len(result.jobids_with("alloc"))
        if n_alloc != 1:
            exc_notes = []
            for jid in result.jobs:
                ev = result.jobs[jid].get("exception")
                if ev is not None:
                    ctx = getattr(ev, "context", {}) or {}
                    exc_notes.append(
                        "jobid {0}: {1}".format(
                            jid,
                            ctx.get("note", ""),
                        )
                    )
            self.fail(
                "GPU job (gpus_per_node={0}) did not reach 'alloc'. "
                "submit_failures={1} exceptions={2}".format(
                    gpus_per_node,
                    result.submit_failures,
                    exc_notes,
                )
            )

    def test_install_one_gpu_per_node_scheduler_allocates(self):
        """gpus_per_node=1 produces R that sched-simple actually
        uses for GPU allocation.

        Regression test: the resource module's ngpus check (in
        test_install_with_gpus) only proves the resource module
        sees the GPUs; it does not exercise the scheduler's
        matching path. A 1-GPU job under gpus_per_node=1 reaches
        'alloc' here if and only if sched-simple matches the
        slot against the GPU in R.
        """
        self._assert_gpu_job_allocates(gpus_per_node=1)

    def test_install_two_gpus_per_node_scheduler_allocates(self):
        """Same as above with gpus_per_node=2 (multi-element
        GPU range). Pairs with the gpus_per_node=1 test so a
        regression specific to single-element ranges shows up
        as one passing and one failing."""
        self._assert_gpu_job_allocates(gpus_per_node=2)

    def test_install_custom_host_prefix(self):
        """install() with custom host_prefix shows up in nodelist"""
        h = flux.Flux()
        InjectFakeResources(
            nodes=3,
            cores_per_node=2,
            host_prefix="node",
        ).install(h)
        rset = flux.resource.resource_list(h).get().all
        self.assertEqual(str(rset.nodelist), "node[0-2]")

    def test_install_with_scheduler_options(self):
        """scheduler_options are parsed via shlex and passed to module.load"""
        h = flux.Flux()
        InjectFakeResources(
            nodes=4,
            cores_per_node=4,
        ).install(
            h,
            scheduler="sched-simple",
            scheduler_options="queue-depth=16",
        )
        # If the options were malformed (e.g. passed as a single
        # string), module.load would have raised. Reaching this
        # point and seeing sched-simple loaded confirms the args
        # array was accepted.
        self.assertEqual(
            ModuleList(h).lookup("sched"),
            "sched-simple",
        )

    def test_log_callable_receives_messages(self):
        """log= callable is invoked with each message string"""
        h = flux.Flux()
        captured = []
        InjectFakeResources(
            nodes=4,
            cores_per_node=2,
            log=captured.append,
        ).install(h)

        # Header line and footer line are always emitted, regardless
        # of verbose. The exact message format may evolve; we just
        # check for stable identifying substrings.
        text = "\n".join(captured)
        self.assertIn("Encoding fake R", text)
        self.assertIn("Fake resources injected", text)

    def test_verbose_emits_trace(self):
        """verbose=True adds module-operation trace lines via log"""
        h = flux.Flux()
        captured = []
        InjectFakeResources(
            nodes=4,
            cores_per_node=2,
            verbose=True,
            log=captured.append,
        ).install(h)

        text = "\n".join(captured)
        self.assertIn("flux module remove", text)
        self.assertIn("flux module reload resource", text)
        self.assertIn("flux module load", text)

    def test_default_log_does_not_write_to_stdout(self):
        """Default log writes to stderr; stdout must stay clean for TAP"""
        # Regression test: an earlier version of this module used
        # `print` (defaulting to stdout) as the log function, which
        # corrupted TAP test output. Stdout must stay empty during a
        # default-config install.
        h = flux.Flux()
        captured_stdout = io.StringIO()
        with contextlib.redirect_stdout(captured_stdout):
            InjectFakeResources(
                nodes=4,
                cores_per_node=2,
                verbose=True,
            ).install(h)

        self.assertEqual(
            captured_stdout.getvalue(),
            "",
            "library must never write to stdout (would break TAP)",
        )


class TestReloadScheduler(_BaseTestCase):
    """reload_scheduler() unloads-then-loads via ModuleList lookup."""

    def test_reload_replaces_existing(self):
        """Calling reload with the loaded scheduler removes and re-adds"""
        # Whatever scheduler the previous test class left loaded
        # (sched-simple) gets replaced via remove + load. The test
        # passes if reload_scheduler succeeds and sched-simple is
        # still loaded afterward.
        h = flux.Flux()
        reload_scheduler(h, "sched-simple")
        self.assertEqual(
            ModuleList(h).lookup("sched"),
            "sched-simple",
        )

    def test_reload_when_none_loaded(self):
        """reload skips remove if no scheduler is currently loaded"""
        h = flux.Flux()
        current = ModuleList(h).lookup("sched")
        if current is not None:
            h.rpc("module.remove", {"name": current}).get()
        self.assertIsNone(ModuleList(h).lookup("sched"))

        reload_scheduler(h, "sched-simple")
        self.assertEqual(
            ModuleList(h).lookup("sched"),
            "sched-simple",
        )

    def test_reload_with_options(self):
        """options string is shlex-split and passed as args array"""
        h = flux.Flux()
        reload_scheduler(h, "sched-simple", options="queue-depth=16")
        self.assertEqual(
            ModuleList(h).lookup("sched"),
            "sched-simple",
        )


if __name__ == "__main__":
    if rerun_under_flux(__flux_size()):
        from pycotap import TAPTestRunner

        unittest.main(testRunner=TAPTestRunner(), buffer=False)
