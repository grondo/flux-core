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
# flux.testing.bulkrun and flux.testing.job_watcher tests
#
# Exercises BulkRun and the two JobEventWatcher implementations
# (JournalEventWatcher, PerJobEventWatcher) against a real Flux
# instance running real jobs.
#

import os
import sys
import unittest

import flux
from flux.job import JobspecV1
from flux.testing.bulkrun import BulkRun
from flux.testing.job_watcher import PerJobEventWatcher
from subflux import rerun_under_flux


def __flux_size():
    return 1


def _trivial_jobspec():
    """Minimal jobspec that runs 'true' and exits immediately."""
    return JobspecV1.from_submit(["true"])


def _sleep_jobspec(seconds):
    """Jobspec that runs 'sleep' for the given duration."""
    return JobspecV1.from_submit(["sleep", str(seconds)])


def _mock_jobspec(runtime="0.001s", args=("sleep", "30")):
    """Jobspec with mock execution.

    Pairs system.exec.test.run_duration with a command that would
    take much longer if actually executed; the broker should honor
    run_duration and finish in simulated time, not actual time. The
    default args of sleep 30 paired with runtime 0.001s give a
    strict test: a broker that ignored run_duration would actually
    run sleep 30 and the bulk would take 30+ seconds per slot.
    """
    return JobspecV1.from_submit(
        list(args),
        attributes={"system.exec.test.run_duration": runtime},
    )


def _wait_for_queue_drain(handle, timeout=30):
    """Block until the broker has no active jobs (queue empty).

    Wraps the ``job-manager.drain`` RPC, the same primitive
    ``flux queue drain`` uses. Used by tests that leave jobs
    running (e.g. via stop()) to ensure cleanup has actually
    completed before returning, rather than relying on an
    arbitrary sleep that may not be long enough on a slow machine.
    """
    future = handle.rpc("job-manager.drain")
    future.wait_for(timeout)
    future.get()


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


class TestBulkRunBasic(_BaseTestCase):
    """Basic submission, observation, and result aggregation."""

    def test_journal_watcher_happy_path(self):
        """20 trivial jobs through the default (journal) watcher reach clean"""
        h = flux.Flux()
        bulk = BulkRun(h)
        bulk.push_jobs(_trivial_jobspec(), 20)
        result = bulk.run()

        self.assertEqual(result.njobs, 20)
        self.assertEqual(len(result.jobids_with("clean")), 20)
        self.assertEqual(len(result.submit_failures), 0)
        self.assertEqual(result.submit_attempted, 20)
        self.assertGreater(result.script_runtime, 0)
        self.assertGreater(
            result.last_event_t("submit") - result.first_event_t("submit"),
            0,
        )
        self.assertGreater(
            result.last_event_t("clean") - result.first_event_t("submit"),
            0,
        )

    def test_per_job_watcher_happy_path(self):
        """Same workload via PerJobEventWatcher; equivalent counts"""
        h = flux.Flux()
        bulk = BulkRun(h, watcher_factory=PerJobEventWatcher)
        bulk.push_jobs(_trivial_jobspec(), 20)
        result = bulk.run()

        self.assertEqual(result.njobs, 20)
        self.assertEqual(len(result.jobids_with("clean")), 20)
        self.assertEqual(len(result.submit_failures), 0)

    def test_journal_inline_R(self):
        """Journal watcher populates inline R on alloc events"""
        h = flux.Flux()
        bulk = BulkRun(h, events_of_interest=("submit", "alloc", "clean"))
        bulk.push_jobs(_trivial_jobspec(), 5)
        result = bulk.run()

        alloc_jobids = result.jobids_with("alloc")
        self.assertGreater(len(alloc_jobids), 0)
        for jobid in alloc_jobids:
            alloc_event = result.jobs[jobid]["alloc"]
            self.assertIsNotNone(getattr(alloc_event, "R", None))

    def test_multi_batch(self):
        """Multiple push_jobs() calls produce a result with batch indices"""
        h = flux.Flux()
        bulk = BulkRun(h)
        idx0 = bulk.push_jobs(_trivial_jobspec(), 5)
        idx1 = bulk.push_jobs(_trivial_jobspec(), 3)
        self.assertEqual(idx0, 0)
        self.assertEqual(idx1, 1)
        result = bulk.run()

        self.assertEqual(result.njobs, 8)
        batch_counts = {0: 0, 1: 0}
        for jobid, evs in result.jobs.items():
            self.assertIn("_batch", evs)
            batch_counts[evs["_batch"]] += 1
        self.assertEqual(batch_counts[0], 5)
        self.assertEqual(batch_counts[1], 3)

    def test_single_batch_omits_batch_key(self):
        """Single-batch runs don't add the _batch key to per-jobid records"""
        h = flux.Flux()
        bulk = BulkRun(h)
        bulk.push_jobs(_trivial_jobspec(), 5)
        result = bulk.run()
        for jobid, evs in result.jobs.items():
            self.assertNotIn("_batch", evs)

    def test_run_with_no_batches(self):
        """run() before push_jobs() raises RuntimeError"""
        h = flux.Flux()
        bulk = BulkRun(h)
        with self.assertRaises(RuntimeError):
            bulk.run()

    def test_run_twice_raises(self):
        """run() called twice raises RuntimeError"""
        h = flux.Flux()
        bulk = BulkRun(h)
        bulk.push_jobs(_trivial_jobspec(), 1)
        bulk.run()
        with self.assertRaises(RuntimeError):
            bulk.run()

    def test_push_after_run_raises(self):
        """push_jobs() after run() raises RuntimeError"""
        h = flux.Flux()
        bulk = BulkRun(h)
        bulk.push_jobs(_trivial_jobspec(), 1)
        bulk.run()
        with self.assertRaises(RuntimeError):
            bulk.push_jobs(_trivial_jobspec(), 1)

    def test_invalid_count_raises(self):
        """push_jobs() with non-positive count raises ValueError"""
        h = flux.Flux()
        bulk = BulkRun(h)
        with self.assertRaises(ValueError):
            bulk.push_jobs(_trivial_jobspec(), 0)
        with self.assertRaises(ValueError):
            bulk.push_jobs(_trivial_jobspec(), -1)


class TestBulkRunCallbacks(_BaseTestCase):
    """add_event_cb, add_bulk_event_cb, and stop()."""

    def test_event_cb_fires_for_every_event(self):
        """add_event_cb is invoked for every event on every tracked job"""
        h = flux.Flux()
        bulk = BulkRun(h)
        seen = []
        bulk.add_event_cb(lambda b, jid, ev: seen.append((jid, ev.name)))
        bulk.push_jobs(_trivial_jobspec(), 5)
        result = bulk.run()

        names_per_jobid = {}
        for jid, name in seen:
            names_per_jobid.setdefault(jid, set()).add(name)
        for jid in result.jobs:
            self.assertIn("submit", names_per_jobid.get(jid, set()))
            self.assertIn("clean", names_per_jobid.get(jid, set()))
        self.assertGreater(len(seen), result.njobs)

    def test_bulk_event_cb_ordering(self):
        """add_bulk_event_cb fires after the last event of the named type"""
        h = flux.Flux()
        bulk = BulkRun(h)

        cleans_seen = [0]
        cleans_seen_at_fire = []

        def on_event(b, jid, ev):
            if ev.name == "clean":
                cleans_seen[0] += 1

        def on_all_clean(b):
            cleans_seen_at_fire.append(cleans_seen[0])

        bulk.add_event_cb(on_event)
        bulk.add_bulk_event_cb("clean", on_all_clean)
        bulk.push_jobs(_trivial_jobspec(), 10)
        result = bulk.run()

        self.assertEqual(
            len(cleans_seen_at_fire),
            1,
            "bulk-event callback should fire exactly once",
        )
        self.assertEqual(
            cleans_seen_at_fire[0],
            result.njobs,
            "bulk-event callback should fire after the last clean",
        )

    def test_bulk_event_cb_multiple_callbacks(self):
        """Multiple callbacks for the same event name all fire"""
        h = flux.Flux()
        bulk = BulkRun(h)
        fired = [0, 0]
        bulk.add_bulk_event_cb("clean", lambda b: fired.__setitem__(0, fired[0] + 1))
        bulk.add_bulk_event_cb("clean", lambda b: fired.__setitem__(1, fired[1] + 1))
        bulk.push_jobs(_trivial_jobspec(), 5)
        bulk.run()
        self.assertEqual(fired, [1, 1])

    def test_stop_returns_promptly(self):
        """stop() from a callback returns from run() before all jobs finish"""
        h = flux.Flux()
        bulk = BulkRun(h)
        submit_count = [0]

        def on_event(b, jid, ev):
            if ev.name == "submit":
                submit_count[0] += 1
                if submit_count[0] >= 5:
                    b.stop()

        bulk.add_event_cb(on_event)
        # Long-running so they cannot finish naturally during the test.
        # Smaller count keeps cleanup quick.
        bulk.push_jobs(_sleep_jobspec(300), 10)
        result = bulk.run()

        try:
            self.assertLess(len(result.jobids_with("clean")), 10)
            self.assertLess(result.script_runtime, 30)
        finally:
            # Cancel leftover jobs and wait for the queue to drain
            # before returning, so the next test's submissions aren't
            # starved for resources held by these jobs.
            bulk.cancelall()
            _wait_for_queue_drain(h)


class TestMockExecution(_BaseTestCase):
    """Verify the broker honors system.exec.test.run_duration."""

    def test_mock_execution_runs(self):
        """Mock-exec jobs reach clean without errors"""
        h = flux.Flux()
        bulk = BulkRun(h)
        bulk.push_jobs(_mock_jobspec(runtime="0.001s"), 10)
        result = bulk.run()
        self.assertEqual(result.njobs, 10)
        self.assertEqual(len(result.jobids_with("clean")), 10)
        self.assertEqual(len(result.submit_failures), 0)

    def test_mock_execution_simulates_time(self):
        """Broker simulates the requested duration, not the real command's"""
        h = flux.Flux()
        bulk = BulkRun(h)
        # Command would take 30s if actually executed; simulated runtime
        # is 1ms. A broker that ignored run_duration would actually run
        # sleep 30 and the bulk would take 30+ seconds.
        bulk.push_jobs(_mock_jobspec(runtime="0.001s", args=("sleep", "30")), 10)
        result = bulk.run()
        self.assertEqual(result.njobs, 10)
        self.assertEqual(len(result.jobids_with("clean")), 10)
        self.assertLess(result.script_runtime, 10.0)

    def test_mock_execution_zero_duration_with_cancel(self):
        """Mock-exec runtime=0 jobs (never finish) terminate via cancel"""
        h = flux.Flux()
        bulk = BulkRun(h, events_of_interest=("submit", "alloc", "clean"))
        bulk.add_bulk_event_cb("alloc", lambda b: b.cancelall())
        bulk.push_jobs(_mock_jobspec(runtime="0", args=("sleep", "30")), 5)
        result = bulk.run()
        self.assertEqual(result.njobs, 5)
        self.assertEqual(len(result.jobids_with("clean")), 5)


class TestBulkRunCancellation(_BaseTestCase):
    """cancelall() end-to-end."""

    def test_cancelall_from_bulk_event_cb(self):
        """cancelall() in a bulk-event callback brings all jobs to clean"""
        h = flux.Flux()
        bulk = BulkRun(h, events_of_interest=("submit", "alloc", "clean"))
        bulk.add_bulk_event_cb("alloc", lambda b: b.cancelall())
        bulk.push_jobs(_sleep_jobspec(300), 5)
        result = bulk.run()

        self.assertEqual(result.njobs, 5)
        self.assertEqual(len(result.jobids_with("clean")), 5)


if __name__ == "__main__":
    if rerun_under_flux(__flux_size()):
        from pycotap import TAPTestRunner

        unittest.main(testRunner=TAPTestRunner(), buffer=False)
