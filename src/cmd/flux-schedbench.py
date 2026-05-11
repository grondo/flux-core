###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""flux schedbench: run scheduler benchmarks, save results, report.

MVP scope: one benchmark per invocation, in the current Flux
instance. The user runs ``flux start`` (or ``flux alloc``)
themselves; ``flux schedbench`` runs inside.

Subcommands:
  run     Run a named benchmark and append the result to the
          results file.
  report  Pretty-print the results file as a table.
"""

import argparse
import getpass
import json
import logging
import socket
import sys
import time

import flux
import flux.resource
import flux.util
from flux.modprobe import ModuleList
from flux.testing.events import (
    NORMAL,
    QUIET,
    VERBOSE,
    TestEventEmitter,
)
from flux.testing.fake_resources import (
    InjectFakeResources,
    saturation_count,
)
from flux.testing.job_watcher import (
    JournalEventWatcher,
    PerJobEventWatcher,
)
from flux.testing.schedbench import BENCHMARKS, BenchmarkResults
from flux.testing.schedbench.ui import TerminalEmitter
from flux.util import OutputFormat, UtilConfig

LOGGER = logging.getLogger("flux-schedbench")
SCHEDBENCH_VERSION = "0.1.0"

#: Maps --watcher NAME to a factory taking a Flux handle. The
#: factory is passed to BulkRun via the benchmark constructor;
#: omitting it (None) keeps BulkRun's default (journal).
_WATCHERS = {
    "journal": lambda h: JournalEventWatcher(h),
    "per-job": lambda h: PerJobEventWatcher(h),
}


def _add_common_run_opts(parser):
    """Add common --opts for 'flux schedbench run' to the parser."""
    parser.add_argument(
        "-N",
        "--nodes",
        type=int,
        default=None,
        metavar="N",
        help="fake-resource node count (required unless --exec; "
        "ignored with --exec — the real broker's resources "
        "are used instead)",
    )
    parser.add_argument(
        "--cores-per-node",
        type=int,
        default=64,
        metavar="C",
        help="cores per node (default: 64; ignored with --exec)",
    )
    parser.add_argument(
        "--gpus-per-node",
        type=int,
        default=8,
        metavar="G",
        help="GPUs per node (default: 8; ignored with --exec)",
    )
    parser.add_argument(
        "--scheduler",
        default="sched-simple",
        metavar="NAME",
        help="scheduler module to load before injecting fake "
        "resources (default: sched-simple). Ignored with "
        "--exec. In both modes, the recorded scheduler "
        "name is read from the broker post-setup via the "
        "sched-service lookup — so the results record "
        "reflects what was actually loaded, not what "
        "was requested.",
    )
    parser.add_argument(
        "--scheduler-options",
        metavar="OPTS",
        help="module options string for the scheduler "
        "(shlex-parsed; ignored with --exec)",
    )
    parser.add_argument(
        "-x",
        "--exec",
        dest="real_exec",
        action="store_true",
        help="run with real job execution (no fake resources, "
        "no system.exec.test.run_duration on jobs); the "
        "broker's currently-loaded scheduler and "
        "currently-up resources are used as-is",
    )
    parser.add_argument(
        "--watcher",
        choices=sorted(_WATCHERS.keys()),
        default="journal",
        help="event-watcher implementation (default: journal). "
        "The journal watcher imposes a single subscription on "
        "kvs-watch; the per-job watcher opens one subscription "
        "per job, useful for measuring watcher overhead under "
        "high job counts.",
    )
    parser.add_argument(
        "--tag",
        metavar="LABEL",
        default="",
        help="free-form label stored in results metadata",
    )
    parser.add_argument(
        "--results-file",
        default="./schedbench-results.json",
        metavar="PATH",
        help="results file path (default: ./schedbench-results.json)",
    )
    parser.add_argument(
        "--no-save",
        action="store_true",
        help="don't append to the results file",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="emit only terminal events (test.start, result, "
        "test.complete, test.error); forces JSON output",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="emit progress/info/metric events",
    )
    parser.add_argument(
        "--ui",
        choices=("auto", "on", "off"),
        default="auto",
        help=(
            "interactive terminal UI: 'auto' (default) enables "
            "it when stdout is a TTY and --quiet is not set; "
            "'on' forces it; 'off' falls back to the JSON event "
            "stream"
        ),
    )
    parser.add_argument(
        "--color",
        choices=("auto", "always", "never"),
        default="auto",
        help=(
            "color output for the terminal UI: 'auto' (default) "
            "honors NO_COLOR and TERM; 'always' / 'never' force"
        ),
    )


def parse_args():
    parser = argparse.ArgumentParser(
        prog="flux-schedbench",
        formatter_class=flux.util.help_formatter(),
        description=(
            "Run scheduler benchmarks against fake resources and "
            "save aggregate metrics"
        ),
    )
    sub = parser.add_subparsers(dest="subcommand")
    sub.required = True  # subparsers(required=) requires Python 3.7+

    # `flux schedbench run TEST ...`
    run_p = sub.add_parser(
        "run",
        help="run a benchmark",
        formatter_class=flux.util.help_formatter(),
    )
    run_p.add_argument(
        "test",
        choices=sorted(BENCHMARKS.keys()),
        help="benchmark to run",
    )
    _add_common_run_opts(run_p)

    # Per-test options (shared on the same subparser; ignored where
    # they don't apply). Documented as such.
    run_p.add_argument(
        "-n",
        "--njobs",
        type=int,
        default=1000,
        metavar="N",
        help="throughput: number of jobs to submit (default: 1000)",
    )
    run_p.add_argument(
        "--slot-size",
        type=int,
        default=1,
        metavar="K",
        help="throughput: cores per slot (default: 1)",
    )
    run_p.add_argument(
        "--slot-cores",
        type=int,
        default=1,
        metavar="K",
        help="fill-machine: cores per slot (default: 1)",
    )
    run_p.add_argument(
        "--slot-gpus",
        type=int,
        default=0,
        metavar="K",
        help="fill-machine: GPUs per slot (default: 0)",
    )

    # `flux schedbench report ...`
    rep_p = sub.add_parser(
        "report",
        help="pretty-print results for a single benchmark",
        formatter_class=flux.util.help_formatter(),
    )
    rep_p.add_argument(
        "test",
        choices=sorted(BENCHMARKS.keys()),
        metavar="TEST",
        help="benchmark to report on (rows of other tests are " "filtered out)",
    )
    rep_p.add_argument(
        "--results-file",
        default="./schedbench-results.json",
        metavar="PATH",
        help="results file path (default: ./schedbench-results.json)",
    )
    rep_p.add_argument(
        "--filter",
        action="append",
        default=[],
        metavar="KEY=VAL",
        help="show only runs matching KEY=VAL; may be given " "multiple times",
    )
    rep_p.add_argument(
        "-o",
        "--format",
        type=str,
        default="default",
        metavar="FORMAT",
        help="output format: a named format ('default', 'long', "
        "'csv', or any user-defined format from "
        "~/.config/flux/flux-schedbench-report-<test>.toml; "
        "'help' lists available names) or a literal "
        "Python-style format string",
    )
    rep_p.add_argument(
        "--sort",
        type=str,
        default="",
        metavar="KEY,...",
        help="sort by one or more keys (prefix with '-' for " "descending)",
    )
    rep_p.add_argument(
        "--no-header",
        action="store_true",
        help="omit the header row",
    )

    args = parser.parse_args()

    # Conditional validation: -N is required unless --exec was
    # given. argparse can't express "required iff another flag
    # is absent", so do it here. Only meaningful for the run
    # subcommand; report doesn't take either flag.
    if args.subcommand == "run":
        if not args.real_exec and args.nodes is None:
            parser.error(
                "-N/--nodes is required (use --exec to skip "
                "fake-resource injection and run against real "
                "broker resources)"
            )

    return args


def _detect_scheduler(handle):
    """Return the name of the module currently providing the
    ``sched`` service, or ``None`` if no scheduler is loaded.

    Scheduler modules (``sched-simple``, ``sched-fluxion-qmanager``,
    etc.) all advertise the ``sched`` service. ``ModuleList``
    indexes loaded modules by their advertised services, so this
    is the single source of truth for "what scheduler is the
    broker actually using right now" — independent of the CLI
    ``--scheduler`` argument, which only controls what
    fake-resource injection asks to load. Used in both modes
    so the recorded ``scheduler.name`` always reflects broker
    reality (catches typos in ``--scheduler``, makes ``--exec``
    records honest about what's loaded).
    """
    return ModuleList(handle).lookup("sched")


def _query_real_resources(handle):
    """Snapshot the broker's currently-up resources.

    Used by ``--exec`` mode to size benchmarks (e.g. fill-
    machine's saturation count) and to fill the resources
    block in the results record with the same shape that
    fake-resources mode produces, so downstream code doesn't
    have to branch on which mode produced the record.

    Caveat: assumes uniform cores/GPUs per node, computing
    averages with floor division. Most HPC allocations are
    uniform; non-uniform setups will see rounded averages.
    ``saturation_count`` will accordingly under-count by up to
    one slot per heterogeneous node — bounded enough that
    aggregate-rate measurements remain meaningful.
    """
    rl = flux.resource.resource_list(handle).get()
    up = rl.up
    nodes = up.nnodes
    if nodes == 0:
        raise RuntimeError(
            "no resources are up on this broker; --exec needs "
            "an allocation with at least one online node"
        )
    return {
        "nodes": nodes,
        "cores_per_node": up.ncores // nodes,
        "gpus_per_node": up.ngpus // nodes,
    }


def _build_benchmark(args, resources):
    """Construct the benchmark instance from CLI args.

    ``resources`` is the resolved shape dict (real-broker query
    or CLI args, depending on mode). Fill-machine uses it to
    compute its saturation count up front so the benchmark
    class itself stays resource-agnostic; throughput ignores it.
    """
    cls = BENCHMARKS[args.test]
    watcher_factory = _WATCHERS[args.watcher]
    if args.test == "throughput":
        return cls(
            njobs=args.njobs,
            slot_cores=args.slot_size,
            watcher_factory=watcher_factory,
            real_exec=args.real_exec,
        )
    if args.test == "fill-machine":
        njobs = saturation_count(
            resources["nodes"],
            resources["cores_per_node"],
            resources["gpus_per_node"],
            slot_cores=args.slot_cores,
            slot_gpus=args.slot_gpus,
        )
        return cls(
            njobs=njobs,
            slot_cores=args.slot_cores,
            slot_gpus=args.slot_gpus,
            watcher_factory=watcher_factory,
            real_exec=args.real_exec,
        )
    raise ValueError(f"unknown benchmark: {args.test}")


class _ResultOnlyEmitter:
    """Emitter for ``--quiet`` on a non-TTY stdout.

    Silently drops every benchmark event except the final
    ``result``, which is printed as a single JSON object on
    stdout. Matches the public surface of TerminalEmitter and
    TestEventEmitter so cmd_run and the benchmark classes don't
    need to know which emitter is in use. Lifecycle and progress
    events are no-ops; errors flow through the normal LOGGER
    path and the process exit code, leaving stdout silent so
    consumers piping to e.g. ``jq`` see either a single result
    object or nothing.
    """

    def __init__(self, stream=None):
        self._stream = stream if stream is not None else sys.stdout

    def test_start(self, test_name, stages, config=None):
        pass

    def stage(self, stage, stage_index, total_stages):
        pass

    def progress(self, current, total, unit, rate=None):
        pass

    def warning(self, message):
        pass

    def info(self, message):
        pass

    def metric(self, name, value, unit=None):
        pass

    def log(self, message):
        pass

    def result(self, metrics):
        # Emit the bare metrics dict, not an event envelope.
        # Downstream tools (jq, plotting scripts) get a clean
        # JSON object with no unwrapping required.
        print(json.dumps(metrics), file=self._stream, flush=True)

    def test_complete(self, duration):
        pass

    def test_error(self, error):
        pass


def _select_emitter(args):
    """Pick the emitter based on resolved UI mode and --quiet.

    Two axes:

    1. ``--ui={auto,on,off}`` chooses between the live
       TerminalEmitter and the JSON-event TestEventEmitter.
       ``auto`` (the default) picks TerminalEmitter iff stdout
       is a TTY, otherwise the JSON stream — so a developer
       gets the rich UI in their terminal but a script
       redirecting stdout gets parseable JSON without
       configuration.

    2. ``--quiet`` narrows the output. On a TTY this still
       gives a human-readable summary — title, config, and the
       results block — but with no progress bars, stage lines,
       or INFO log messages. Off a TTY ``--quiet`` produces a
       single JSON object (the metrics dict) and nothing else;
       errors go to stderr only.

    The TerminalEmitter consults its ``verbosity`` to decide
    whether to render mid-run; passing QUIET there gives the
    quiet-summary behavior. Off-TTY quiet uses the dedicated
    :class:`_ResultOnlyEmitter` rather than a verbosity tweak
    on TestEventEmitter because the goal is a single
    ``{...}`` line on stdout, not a stripped-down event
    stream.
    """
    ui = args.ui
    if ui == "auto":
        ui = "on" if sys.stdout.isatty() else "off"

    if ui == "on":
        verbosity = QUIET if args.quiet else VERBOSE
        return TerminalEmitter(verbosity=verbosity, color=args.color)

    # ui == "off"
    if args.quiet:
        return _ResultOnlyEmitter()
    return TestEventEmitter(verbosity=NORMAL)


def _flux_version(handle):
    """Best-effort: return broker version string, or None if unavailable."""
    try:
        return handle.attr_get("version")
    except (OSError, AttributeError):
        return None


def _config_dict(args, resources, scheduler_name):
    """The test.start event's context['config']: human-readable
    parameters of this run.

    ``resources`` and ``scheduler_name`` are the resolved
    values from the broker (post-setup), not the CLI defaults.
    Building the dict from them rather than from ``args.nodes``
    / ``args.scheduler`` keeps the TerminalEmitter header
    accurate under ``--exec``, where the CLI values are
    unspecified and the real ones come from
    :func:`_query_real_resources` and :func:`_detect_scheduler`.
    """
    cfg = {
        "nodes": resources["nodes"],
        "cores_per_node": resources["cores_per_node"],
        "gpus_per_node": resources["gpus_per_node"],
        "scheduler": scheduler_name,
        "watcher": args.watcher,
        "real_exec": args.real_exec,
    }
    if args.test == "throughput":
        cfg.update(
            njobs=args.njobs,
            slot_size=args.slot_size,
        )
    elif args.test == "fill-machine":
        cfg.update(
            slot_cores=args.slot_cores,
            slot_gpus=args.slot_gpus,
        )
    return cfg


def _run_record(args, resources, scheduler_name, metrics, flux_version):
    """Build the result-file record for this run.

    ``resources`` carries the resolved shape (real or fake);
    ``scheduler_name`` is the broker's currently-loaded sched
    module, looked up post-setup (so it matches what actually
    ran, not what was requested); ``args.real_exec`` flags
    which mode produced the run. Schema is identical for both
    modes so reports and downstream tools don't have to branch.
    """
    return {
        "test_name": args.test,
        "tag": args.tag,
        "host": socket.gethostname(),
        "user": getpass.getuser(),
        "schedbench_version": SCHEDBENCH_VERSION,
        "flux_core_version": flux_version,
        "real_exec": args.real_exec,
        "scheduler": {
            "name": scheduler_name,
            "options": args.scheduler_options or "",
            "version": None,
        },
        "resources": dict(resources),
        "watcher": args.watcher,
        "benchmarks": {
            args.test: {"results": metrics},
        },
    }


def cmd_run(args):
    """Execute a single benchmark run.

    Two execution paths:

    * Mock (default): inject fake resources and configure the
      scheduler, then run with the test exec implementation
      via ``simple_jobspec(mock=True)``.

    * Real (``--exec``): leave the broker's resources and
      scheduler as the user set them up; query the resource
      state for sizing and provenance, then run real jobs.

    Both paths converge on the same ``resources`` shape dict
    plus a ``scheduler_name`` resolved from broker truth, and
    flow through the same benchmark / emitter / results
    pipeline.
    """
    handle = flux.Flux()
    if args.real_exec:
        # User is responsible for resources and scheduler in
        # the existing broker. We just observe.
        resources = _query_real_resources(handle)
    else:
        fake = InjectFakeResources(
            nodes=args.nodes,
            cores_per_node=args.cores_per_node,
            gpus_per_node=args.gpus_per_node,
            verbose=args.verbose,
            log=LOGGER.info,
        )
        fake.install(
            handle,
            scheduler=args.scheduler,
            scheduler_options=args.scheduler_options,
        )
        resources = {
            "nodes": fake.nodes,
            "cores_per_node": fake.cores_per_node,
            "gpus_per_node": fake.gpus_per_node,
        }

    # Always resolve scheduler from broker state. In mock mode
    # this confirms (and records) what fake.install actually
    # loaded; in --exec mode it's the only source of truth — and
    # the absence of a sched-service provider is fatal there
    # since the benchmark would otherwise hang waiting for
    # allocations the broker can never make.
    scheduler_name = _detect_scheduler(handle)
    if scheduler_name is None:
        if args.real_exec:
            raise RuntimeError(
                "--exec requires a scheduler module to be loaded "
                "(no module currently provides the 'sched' "
                "service). Load one with `flux module load "
                "sched-simple` or equivalent before running."
            )
        # Mock mode: fake.install just loaded args.scheduler, so
        # ModuleList should see it. Defensive fallback in case
        # the module list hasn't refreshed.
        scheduler_name = args.scheduler

    bench = _build_benchmark(args, resources)

    emitter = _select_emitter(args)
    emitter.test_start(
        args.test,
        stages=bench.stages,
        config=_config_dict(args, resources, scheduler_name),
    )

    t0 = time.monotonic()
    try:
        metrics = bench.run(handle, emitter)
    except Exception as exc:  # noqa: BLE001
        emitter.test_error(str(exc))
        raise
    duration = time.monotonic() - t0

    emitter.result(metrics)
    emitter.test_complete(duration=duration)

    if not args.no_save:
        results = BenchmarkResults(args.results_file)
        results.add_run(
            _run_record(
                args,
                resources,
                scheduler_name,
                metrics,
                _flux_version(handle),
            ),
        )
        results.save()


def _parse_filters(filter_args):
    """Parse --filter KEY=VAL strings into a list of (key, val) pairs."""
    parsed = []
    for f in filter_args:
        if "=" not in f:
            raise ValueError(f"--filter must be KEY=VAL form, got: {f!r}")
        key, val = f.split("=", 1)
        parsed.append((key, val))
    return parsed


def _matches_filters(run, filters):
    """Does ``run`` match every ``(key, val)`` filter?"""
    for key, val in filters:
        if str(run.get(key)) != val:
            return False
    return True


class _BenchmarkReportConfig(UtilConfig):
    """User-customizable named-format registry for one benchmark.

    Wraps :class:`flux.util.UtilConfig` so the benchmark's
    builtin ``REPORT_FORMATS`` are available to
    :meth:`get_format_string` along with any user-defined
    formats loaded from ``~/.config/flux/flux-schedbench-
    report-<benchmark>.toml``. Each benchmark gets its own
    config namespace so users adding a ``myformat`` for
    throughput don't pollute fill-machine's report.

    Auto-generates a ``csv`` format from the benchmark's
    ``REPORT_HEADINGS`` when the benchmark doesn't define one
    explicitly: this keeps CSV output in sync with the field
    schema without per-benchmark duplication. Benchmarks that
    want different column ordering can override ``csv`` in
    their ``REPORT_FORMATS``.
    """

    def __init__(self, bench_cls):
        formats = dict(bench_cls.REPORT_FORMATS)
        if "csv" not in formats:
            formats["csv"] = {
                "description": "All fields, comma-separated (for plotting)",
                "format": _csv_format_for(bench_cls.REPORT_HEADINGS),
            }
        super().__init__(
            name=f"flux-schedbench-report-{bench_cls.name}",
            initial_dict={"formats": formats},
        )

    def validate(self, path, config):
        for key, value in config.items():
            if key == "formats":
                self.validate_formats(path, value)
            else:
                raise ValueError(f"{path}: invalid key {key}")


def _csv_format_for(headings):
    """Build a CSV format string from a benchmark's headings.

    Returns ``"{f1},{f2},...,{fN}"`` for each key in headings,
    preserving insertion order. No format specs are used so
    missing/None values render as empty cells (per ``""`` in
    :data:`OutputFormat.empty_outputs`), which is exactly what
    spreadsheets and pandas expect.
    """
    return ",".join(f"{{{k}}}" for k in headings)


class _ReportRow:
    """Flat attribute view of a results-file run for OutputFormat.

    Every field named in the benchmark's ``REPORT_HEADINGS`` is
    initialized to ``""`` so OutputFormat's ``?:`` sentinel
    works (the empty-string check in ``empty_outputs()`` picks
    it up) and CSV cells for missing data render empty rather
    than ``"None"``. Values are then overwritten from the run
    record where applicable. The row never raises
    ``AttributeError`` for a heading-listed field — older
    results that predate a metric just get ``""`` for it.
    """

    def __init__(self, run, bench_cls):
        for key in bench_cls.REPORT_HEADINGS:
            setattr(self, key, "")
        self.time = run.get("iso_timestamp", "")
        sched = run.get("scheduler") or {}
        self.scheduler = sched.get("name", "")
        self.tag = run.get("tag", "")
        self.watcher = run.get("watcher", "")
        res = run.get("resources") or {}
        self.nodes = res.get("nodes", "")
        self.cores = res.get("cores_per_node", "")
        self.gpus = res.get("gpus_per_node", "")
        # real_exec is a boolean in the JSON; rendered as Y/N
        # so the column reads at a glance and is consistent in
        # CSV too (no NaN cells for pandas). Older records
        # without the field render as "N" — correct, since the
        # feature postdates them and those runs were all mock.
        self.real_exec = "Y" if run.get("real_exec") else "N"
        # Per-test metric dict lives at benchmarks[test_name].results.
        # Use the run's recorded test_name (not bench_cls.name) so a
        # rename of the benchmark class doesn't strand old records.
        test = run.get("test_name", "")
        metrics = (run.get("benchmarks") or {}).get(test, {}).get("results", {})
        for key, value in metrics.items():
            if key in bench_cls.REPORT_HEADINGS:
                setattr(self, key, value)

        # Final normalization: convert any None to "". Two ways
        # None leaks into a row: (a) argparse stores unset string
        # options like --tag as None, which _run_record propagates
        # to the JSON; .get(key, default) only returns default
        # when the key is *absent*, not when its value is
        # explicitly None — so tag-less runs leave self.tag=None
        # here even though we passed "" as the default. (b) An
        # older results file may have a metric stored as None.
        # Both cases would otherwise blow up at format time
        # because numeric/precision specs reject None (and even
        # the string spec "<8.8" fails with "unsupported format
        # string passed to NoneType.__format__"). Defending here
        # keeps the renderer simple and tolerant.
        for key in bench_cls.REPORT_HEADINGS:
            if getattr(self, key) is None:
                setattr(self, key, "")


def cmd_report(args):
    """Pretty-print results for a single benchmark.

    Loads the benchmark's report config (builtin + user TOML),
    resolves ``-o`` against named formats, applies ``--sort``
    and ``--filter``, and delegates to
    :meth:`OutputFormat.print_items` for rendering. An implicit
    filter restricts rows to the requested benchmark so the
    output is always per-test (no cross-benchmark column
    confusion).
    """
    bench_cls = BENCHMARKS[args.test]

    results = BenchmarkResults(args.results_file)
    runs = results.get_runs()

    filters = _parse_filters(args.filter)
    filters.append(("test_name", args.test))
    runs = [r for r in runs if _matches_filters(r, filters)]

    if not runs:
        LOGGER.error(
            "no %s runs match the given filters",
            args.test,
        )
        sys.exit(1)

    config = _BenchmarkReportConfig(bench_cls).load()
    try:
        fmt_str = config.get_format_string(args.format)
        formatter = OutputFormat(
            fmt_str,
            headings=bench_cls.REPORT_HEADINGS,
        )
    except ValueError as err:
        raise ValueError(f"invalid format: {err}")

    if args.sort:
        try:
            formatter.set_sort_keys(args.sort)
        except ValueError as err:
            raise ValueError(f"invalid sort key: {err}")

    rows = [_ReportRow(r, bench_cls) for r in runs]
    formatter.print_items(rows, no_header=args.no_header)


@flux.util.CLIMain(LOGGER)
def main():
    args = parse_args()
    # `run` accepts -q/--quiet but `report` does not, so guard
    # the attribute lookup. When set, raise the log level past
    # INFO so callback-based info logging from helpers (e.g.
    # InjectFakeResources.log=LOGGER.info) goes silent. WARNING
    # and above still pass through — those are real issues the
    # user needs to see regardless of quiet mode. Results-bearing
    # events flow through the emitter, not the logger, so the
    # result event is unaffected.
    if getattr(args, "quiet", False):
        LOGGER.setLevel(logging.WARNING)
    if args.subcommand == "run":
        cmd_run(args)
    elif args.subcommand == "report":
        cmd_report(args)


if __name__ == "__main__":
    main()

# vi: ts=4 sw=4 expandtab
