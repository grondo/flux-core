###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Terminal UI emitter for flux-schedbench.

Drop-in alternative to :class:`flux.testing.events.TestEventEmitter`
for interactive use. Same public method surface (``test_start``,
``stage``, ``progress``, ``result``, ``test_complete``,
``test_error``, ``info``, ``warning``, ``metric``, ``log``) — the
benchmark code is identical; the CLI picks an emitter at startup
based on TTY detection and the ``--ui`` flag.

Rendering layout::

    flux schedbench · <test_name>
      <resource summary>
      <jobspec summary> · <scheduler> · <watcher>

      <glyph> <stage>  <bar>  <count>  <elapsed>  <rate>
      ...

      <metrics table on completion>
      <elapsed> elapsed

The block re-renders in place via ANSI cursor moves (cursor-up
then erase-to-end-of-screen). Redraws throttle to roughly 20Hz
to avoid flicker; a final render is always forced on completion
or error so the latest counts are visible.

Colors are 16-color ANSI only, opt-out via ``NO_COLOR``,
``--color=never``, or non-TTY ``stream``. No external dependencies.
"""

import os
import sys
import time

from flux.testing.events import _EVENT_MIN_VERBOSITY, QUIET, VERBOSE

# Stage status glyphs. Picked to render cleanly on any terminal
# that handles UTF-8 (every modern one); see _safe_glyphs() for
# the ASCII fallback path.
_GLYPH_PENDING = "·"
_GLYPH_ACTIVE = "▶"
_GLYPH_DONE = "✓"
_GLYPH_FAILED = "✗"

# Progress bar cells. Solid block for completed portion, light
# shade for the remainder.
_BAR_FILL = "█"
_BAR_EMPTY = "░"

# ANSI escape sequences. Kept inline (one constant per code) so
# readers don't need to remember escape numbers.
_ESC = "\x1b["
_RESET = _ESC + "0m"
_BOLD = _ESC + "1m"
_DIM = _ESC + "2m"
_FG_RED = _ESC + "31m"
_FG_GREEN = _ESC + "32m"
_FG_YELLOW = _ESC + "33m"
_FG_CYAN = _ESC + "36m"

# Cursor up N lines (parameterized at call site), erase from
# cursor to end of screen.
_CURSOR_UP_FMT = _ESC + "{0}A"
_ERASE_TO_END = _ESC + "J"
# Hide / show cursor across the redraw loop so the terminal
# doesn't flicker a cursor mid-line on each repaint.
_CURSOR_HIDE = _ESC + "?25l"
_CURSOR_SHOW = _ESC + "?25h"

# Minimum delay between repaints, in seconds. ~20Hz is fast
# enough to feel responsive without spending the program's whole
# time budget in render code or flickering on slow terminals.
_REDRAW_INTERVAL_S = 0.05

# Layout constants. Stage names cap at 7 chars ("cleanup");
# count fits "1024/1024" = 9; rate fits "5,000 jobs/sec" with
# room to grow. Bar grows up to 40 cells on wide terminals
# (was 28 — needlessly tight).
_STAGE_NAME_WIDTH = 7
_COUNT_WIDTH = 9
_DEFAULT_WIDTH = 80
_MIN_BAR_WIDTH = 8
_MAX_BAR_WIDTH = 40
_ELAPSED_WIDTH = 6
_RATE_WIDTH = 14


def _isatty(stream):
    try:
        return stream.isatty()
    except (AttributeError, ValueError):
        return False


def _color_supported(stream, color):
    """Resolve ``--color={auto,always,never}`` against the
    environment. Returns True iff colored escape sequences
    should be emitted on ``stream``."""
    if color == "never":
        return False
    if color == "always":
        return True
    # auto
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    return _isatty(stream)


def _safe_glyphs():
    """Return True if the terminal can render the Unicode glyphs
    we use; otherwise the caller falls back to ASCII. UTF-8 is
    the only safe assumption; check the relevant env vars."""
    for var in ("LC_ALL", "LC_CTYPE", "LANG"):
        v = os.environ.get(var, "")
        if "UTF-8" in v.upper() or "UTF8" in v.upper():
            return True
    return False


class TerminalEmitter:
    """Render schedbench events as a live multi-line terminal UI.

    Implements the same public methods as
    :class:`flux.testing.events.TestEventEmitter` so the CLI can
    pick either emitter at startup without the benchmark code
    knowing the difference.

    Args:
      verbosity: filter threshold; events below the threshold
          are dropped. Defaults to :data:`VERBOSE` since the UI
          needs ``progress`` and ``metric`` to display anything
          interesting.
      stream: file-like object that receives the rendered UI.
          Defaults to :data:`sys.stdout`.
      color: ``"auto"`` (default), ``"always"``, or ``"never"``.
      ascii_only: force ASCII glyphs and bar characters. If
          omitted, autodetects via locale env vars.
    """

    __test__ = False

    def __init__(self, verbosity=VERBOSE, stream=None, color="auto", ascii_only=None):
        self.verbosity = verbosity
        self._stream = stream if stream is not None else sys.stdout
        self._color = _color_supported(self._stream, color)
        self._ascii = ascii_only if ascii_only is not None else not _safe_glyphs()

        # Run state.
        self._test_name = None
        self._config = None
        self._stage_names = []
        self._stages = {}  # name → dict of state
        self._current_stage = None
        self._metrics = None
        self._error = None
        self._start_time = None
        self._end_time = None

        # Rendering state.
        self._last_render_lines = 0
        self._last_render_t = 0.0
        self._cursor_hidden = False

    # ----- Glyph / color helpers (resolve once per draw) -----

    def _glyph(self, name):
        if self._ascii:
            return {
                "pending": ".",
                "active": ">",
                "done": "+",
                "failed": "x",
            }[name]
        return {
            "pending": _GLYPH_PENDING,
            "active": _GLYPH_ACTIVE,
            "done": _GLYPH_DONE,
            "failed": _GLYPH_FAILED,
        }[name]

    def _bar_chars(self):
        if self._ascii:
            return "#", "-"
        return _BAR_FILL, _BAR_EMPTY

    def _c(self, code, text):
        """Wrap ``text`` in ANSI color ``code`` iff color is on."""
        if not self._color:
            return text
        return code + text + _RESET

    # ----- Public API matching TestEventEmitter -----

    def emit(self, name, context=None):
        """Generic event hook. Mostly unused; the typed methods
        below cover the schedbench event names. Kept so consumers
        that call ``emitter.emit("info", ...)`` directly still
        work."""
        if _EVENT_MIN_VERBOSITY.get(name, QUIET) > self.verbosity:
            return
        # Dispatch to the typed entry points so state updates
        # flow through the same paths regardless of caller style.
        ctx = context or {}
        if name == "test.start":
            self.test_start(
                ctx.get("test_name", "?"),
                ctx.get("stages", []),
                ctx.get("config"),
            )
        elif name == "stage":
            self.stage(
                ctx.get("stage", ""),
                ctx.get("stage_index", 0),
                ctx.get("total_stages", 1),
            )
        elif name == "progress":
            self.progress(
                ctx.get("current", 0),
                ctx.get("total", 0),
                ctx.get("unit", ""),
                ctx.get("rate"),
            )
        elif name == "result":
            self.result(ctx.get("metrics", {}))
        elif name == "test.complete":
            self.test_complete(ctx.get("duration", 0.0))
        elif name == "test.error":
            self.test_error(ctx.get("error", ""))

    def test_start(self, test_name, stages, config=None):
        self._test_name = test_name
        self._stage_names = list(stages)
        self._stages = {
            s: {
                "current": 0,
                "total": 0,
                "unit": "",
                "started": None,
                "finished": None,
                "status": "pending",
            }
            for s in self._stage_names
        }
        self._config = config or {}
        self._start_time = time.time()
        # In quiet mode we render only at test_complete /
        # test_error, so we never paint mid-run and don't need
        # to hide the cursor. _redraw below is a no-op at QUIET.
        if self.verbosity != QUIET and _isatty(self._stream):
            self._stream.write(_CURSOR_HIDE)
            self._cursor_hidden = True
        self._redraw(force=True)

    def stage(self, stage, stage_index, total_stages):
        del stage_index, total_stages  # available in _stage_names
        # Mark previous active stage as done (it transitioned out
        # of being current). The benchmarks emit each stage in
        # order, so anything we entered before this call has
        # finished its work even if its progress count didn't
        # reach total (e.g., the cancel stage in fill-machine
        # has no progress events).
        if self._current_stage is not None:
            prev = self._stages[self._current_stage]
            prev["status"] = "done"
            prev["finished"] = time.time()
        self._current_stage = stage
        if stage in self._stages:
            self._stages[stage]["status"] = "active"
            self._stages[stage]["started"] = time.time()
        self._redraw(force=True)

    def progress(self, current, total, unit, rate=None):
        del rate  # we compute it from elapsed
        if _EVENT_MIN_VERBOSITY["progress"] > self.verbosity:
            return
        if self._current_stage is None:
            return
        st = self._stages[self._current_stage]
        st["current"] = current
        st["total"] = total
        st["unit"] = unit
        self._redraw()

    def result(self, metrics):
        self._metrics = dict(metrics)
        # The final render happens on test_complete/test_error;
        # don't redraw yet (avoids a flash of partial metrics
        # alongside a still-active progress bar).

    def test_complete(self, duration):
        del duration  # we compute it from _start_time/_end_time
        if self._current_stage is not None:
            st = self._stages[self._current_stage]
            if st["status"] == "active":
                st["status"] = "done"
                st["finished"] = time.time()
        self._end_time = time.time()
        self._redraw(force=True, final=True)
        self._show_cursor()

    def test_error(self, error):
        self._error = error
        if self._current_stage is not None:
            st = self._stages[self._current_stage]
            if st["status"] == "active":
                st["status"] = "failed"
                st["finished"] = time.time()
        self._end_time = time.time()
        self._redraw(force=True, final=True)
        self._show_cursor()

    def info(self, message):
        del message  # informational chatter doesn't appear in the UI

    def warning(self, message):
        # Warnings are above-the-UI noise; print to stderr so they
        # don't disrupt the redrawing block on stdout.
        print("warning: " + str(message), file=sys.stderr, flush=True)

    def metric(self, name, value, unit=None):
        del name, value, unit  # intermediate metrics not displayed

    def log(self, message):
        # Free-form log lines go to stderr (same policy as
        # TestEventEmitter) so they don't disturb the redrawing
        # stdout block.
        print(message, file=sys.stderr, flush=True)

    # ----- Internal: rendering -----

    def _show_cursor(self):
        if self._cursor_hidden and _isatty(self._stream):
            self._stream.write(_CURSOR_SHOW)
            self._stream.flush()
            self._cursor_hidden = False

    def _redraw(self, force=False, final=False):
        # In quiet mode we want exactly one render — the final
        # results block — so suppress every mid-run paint. The
        # final test_complete / test_error call passes
        # final=True to bypass this gate. Nothing else does.
        if self.verbosity == QUIET and not final:
            return

        now = time.time()
        if not force and (now - self._last_render_t) < _REDRAW_INTERVAL_S:
            return
        self._last_render_t = now

        lines = self._render_lines()

        # Rewind to the top of the previous block and erase
        # everything below the cursor before writing the new
        # block. Skip the rewind for the very first render.
        if self._last_render_lines > 0 and _isatty(self._stream):
            self._stream.write(_CURSOR_UP_FMT.format(self._last_render_lines))
            self._stream.write(_ERASE_TO_END)

        out = "\n".join(lines) + "\n"
        self._stream.write(out)
        self._stream.flush()
        self._last_render_lines = len(lines)

    def _term_width(self):
        """Return the actual terminal width by querying the
        stream's controlling TTY directly. We deliberately do
        not use ``shutil.get_terminal_size``, which prefers the
        ``COLUMNS`` environment variable — that can be stale
        when the schedbench command runs as a subprocess (e.g.,
        under ``flux start``) inheriting an env var the parent
        shell set for a different window size. ``os.get_terminal_size``
        on the stream's fd goes straight to TIOCGWINSZ and gets
        the real width.
        """
        try:
            cols = os.get_terminal_size(self._stream.fileno()).columns
        except (AttributeError, ValueError, OSError):
            cols = _DEFAULT_WIDTH
        return max(cols, 40)

    def _render_lines(self):
        """Produce the full block as a list of lines (no
        trailing newlines on individual entries; the writer
        joins with ``\n`` and appends one trailing newline)."""
        out = []

        # Header.
        sep = "·" if not self._ascii else "-"
        if self._test_name is not None:
            title = (
                self._c(_BOLD, "flux schedbench") + " " + sep + " " + self._test_name
            )
            out.append(title)

        if self._config:
            cfg = self._config
            res_parts = []
            if "nodes" in cfg:
                res_parts.append(_pluralize(cfg["nodes"], "node"))
            if "cores_per_node" in cfg:
                res_parts.append(_pluralize(cfg["cores_per_node"], "core"))
            if cfg.get("gpus_per_node"):
                res_parts.append(_pluralize(cfg["gpus_per_node"], "GPU"))
            misc = []
            if "scheduler" in cfg:
                misc.append(cfg["scheduler"])
            if "watcher" in cfg:
                misc.append("{0} watcher".format(cfg["watcher"]))
            # "real execution" annotates the noteworthy case
            # only; mock is the default and stays unannotated
            # to keep the header tight. The flag also lands in
            # the results JSON and the REAL report column, so
            # this is a glance-level cue rather than the
            # primary record.
            if cfg.get("real_exec"):
                misc.append("real execution")
            x = " x " if self._ascii else " × "
            res_line = "  " + x.join(res_parts)
            if misc:
                res_line += " " + sep + " " + (" " + sep + " ").join(misc)
            out.append(self._c(_DIM, res_line))

        out.append("")

        # Stage lines. In quiet mode the entire stage block is
        # omitted — the user asked not to see progress, and the
        # results summary below carries the per-rate/timing
        # numbers that make the stage block redundant.
        if self.verbosity != QUIET:
            width = self._term_width()
            for name in self._stage_names:
                out.append(self._render_stage_line(name, width))

            out.append("")

        # Metrics (final view only).
        if self._metrics:
            for line in self._render_metrics_lines():
                out.append(line)
            out.append("")

        # Error block.
        if self._error:
            for line in self._error.splitlines():
                out.append("  " + self._c(_FG_RED, line))
            out.append("")

        # Footer: elapsed.
        elapsed = self._elapsed_str()
        if elapsed:
            out.append(self._c(_DIM, "  " + elapsed))

        return out

    def _render_stage_line(self, stage, width):
        st = self._stages[stage]
        status = st["status"]

        if status == "done":
            glyph = self._c(_FG_GREEN, self._glyph("done"))
            name_text = stage
        elif status == "active":
            glyph = self._c(_FG_YELLOW, self._glyph("active"))
            name_text = self._c(_BOLD, stage)
        elif status == "failed":
            glyph = self._c(_FG_RED, self._glyph("failed"))
            name_text = self._c(_FG_RED, stage)
        else:
            glyph = self._c(_DIM, self._glyph("pending"))
            name_text = self._c(_DIM, stage)

        # Stage name column. Pad based on display width (color
        # codes don't count toward padding).
        pad = max(0, _STAGE_NAME_WIDTH - len(stage))
        name_col = name_text + (" " * pad)

        # Count and bar.
        current = st["current"]
        total = st["total"]
        if total > 0:
            count_str = "{0}/{1}".format(current, total)
        else:
            count_str = ""
        count_col = count_str.rjust(_COUNT_WIDTH)

        # Compute bar width. The fixed columns account for every
        # inter-column 2-space gap plus the four fixed-width
        # columns. The bar grows up to _MAX_BAR_WIDTH on wide
        # terminals. We stop 1 char short of the reported width
        # to avoid the VT100 "phantom column": writing exactly
        # into the rightmost column doesn't wrap until the next
        # character — terminals disagree about whether \n in
        # that state acts as one or two newlines. Staying under
        # the edge sidesteps the question.
        fixed = (
            2  # leading indent
            + 1
            + 1  # glyph + space
            + _STAGE_NAME_WIDTH
            + 2  # gap: name → bar
            + 2  # gap: bar → count
            + _COUNT_WIDTH
            + 2  # gap: count → elapsed
            + _ELAPSED_WIDTH
            + 2  # gap: elapsed → rate
            + _RATE_WIDTH
            + 1  # phantom-column safety margin
        )
        bar_width = max(
            _MIN_BAR_WIDTH,
            min(_MAX_BAR_WIDTH, width - fixed),
        )
        bar_col = self._render_bar(current, total, bar_width, status)

        # Elapsed and rate (only when meaningful). Rate keeps the
        # full unit string from the progress event ("832 job/s")
        # instead of a one-letter abbreviation — the column is
        # wide enough now.
        elapsed_col = ""
        rate_col = ""
        if st["started"] is not None:
            end = st["finished"] if st["finished"] else time.time()
            dt = end - st["started"]
            elapsed_col = "{0:5.2f}s".format(dt)
            if current > 0 and dt > 0 and status in ("active", "done"):
                unit = st["unit"] or ""
                if unit:
                    rate_col = "{0:5.0f} {1}/s".format(
                        current / dt,
                        unit,
                    )
                else:
                    rate_col = "{0:5.0f} /s".format(current / dt)

        # Trailing whitespace on empty trailing columns (pending
        # stages have no elapsed/rate) was the second half of the
        # wrap bug — even after fixing the bar-width math, an
        # empty rate column padded to _RATE_WIDTH adds 14 spaces
        # of bloat per pending line. rstrip handles that.
        #
        # The rate column is *left*-justified within _RATE_WIDTH
        # so the numeric portion lines up across stages even when
        # the unit string lengths differ. With rjust, a stage
        # using "job/s" (5 chars) would have its number pushed
        # 3 columns right of a stage using "cancel/s" (8 chars),
        # since both strings right-end at the same column. With
        # ljust, the number's column is fixed by the "{:>5.0f}"
        # prefix and only the unit string extends to the right.
        # Line-level rstrip removes the ljust padding so the
        # phantom-column safety margin still works.
        line = (
            "  "
            + glyph
            + " "
            + name_col
            + "  "
            + bar_col
            + "  "
            + count_col
            + "  "
            + elapsed_col.rjust(_ELAPSED_WIDTH)
            + "  "
            + rate_col.ljust(_RATE_WIDTH)
        )
        return line.rstrip()

    def _render_bar(self, current, total, width, status):
        fill, empty = self._bar_chars()
        # A done stage renders a fully-filled bar regardless of
        # the recorded count. Some stages (e.g. fill-machine's
        # cancel stage) emit no progress events, so total stays
        # at 0; showing an empty bar there would suggest nothing
        # happened. "Done" is the source of truth here.
        if status == "done":
            return self._c(_FG_GREEN, fill * width)
        if total <= 0:
            ratio = 0.0
        else:
            ratio = min(1.0, current / total)
        filled = int(round(ratio * width))
        bar = fill * filled + empty * (width - filled)
        if status == "active":
            return self._c(_FG_CYAN, bar)
        if status == "failed":
            return self._c(_FG_RED, bar)
        return self._c(_DIM, bar)

    def _render_metrics_lines(self):
        """Format the final metrics table.

        Only keys in :data:`_METRIC_DISPLAY` are shown — raw
        ``t_*`` timestamps and other internal fields are
        dropped here (they're still preserved in the results
        JSON file). Each row has three columns: label (left-
        aligned, padded to the longest label), number (right-
        aligned, padded to the longest number), unit (dim,
        left-aligned).
        """
        rows = []
        for key, label, kind in _METRIC_DISPLAY:
            if key not in self._metrics:
                continue
            num, unit = _format_metric(self._metrics[key], kind)
            rows.append((label, num, unit))
        if not rows:
            return []

        label_w = max(len(r[0]) for r in rows)
        num_w = max(len(r[1]) for r in rows)
        out = []
        for label, num, unit in rows:
            line = (
                "  "
                + label.ljust(label_w + 2)
                + self._c(_BOLD, num.rjust(num_w))
                + "  "
                + self._c(_DIM, unit)
            )
            out.append(line.rstrip())
        return out

    def _elapsed_str(self):
        if self._start_time is None:
            return ""
        end = self._end_time if self._end_time else time.time()
        dt = end - self._start_time
        sep = " · " if not self._ascii else " - "
        suffix = " elapsed"
        if self._end_time and self._error is None:
            suffix += sep + self._c(_FG_GREEN, "ok")
        elif self._end_time and self._error is not None:
            suffix += sep + self._c(_FG_RED, "failed")
        return "{0:.2f}s".format(dt) + suffix


#: Curated set of result-dict keys to display in the post-run
#: metrics table, in order. Anything not listed here (e.g. raw
#: ``t_*`` timestamps) is silently skipped; those values are
#: still preserved in the results JSON file for analysis.
#: ``kind`` selects the formatter / unit in :func:`_format_metric`.
_METRIC_DISPLAY = (
    ("njobs", "jobs", "count"),
    ("time_to_fill", "fill time", "seconds"),
    ("throughput", "throughput", "rate"),
    ("script_throughput", "script throughput", "rate"),
    ("submit_rate", "submit rate", "rate"),
    ("alloc_rate", "alloc rate", "rate"),
    ("start_rate", "start rate", "rate"),
    ("cancel_rate", "cancel rate", "rate"),
)


def _format_metric(value, kind):
    """Format ``value`` for display in the metrics table.

    Returns a ``(number, unit)`` tuple so the renderer can right-
    align the number column independently of the unit column.
    Rate precision adapts to magnitude: rates above 1000 don't
    need decimal places, while rates below 100 benefit from two.
    """
    if kind == "count":
        # Label is "jobs"; suppressing the unit avoids a
        # redundant "jobs ... jobs" row in the metrics table.
        return ("{0:,}".format(int(value)), "")
    if kind == "seconds":
        return ("{0:.2f}".format(float(value)), "s")
    if kind == "rate":
        v = float(value)
        if v >= 1000:
            return ("{0:,.0f}".format(v), "job/s")
        if v >= 100:
            return ("{0:,.1f}".format(v), "job/s")
        return ("{0:,.2f}".format(v), "job/s")
    # Fallback for unknown kinds: stringify with no unit.
    return (str(value), "")


def _pluralize(n, label):
    """Format ``"{n} {label}[s]"`` with English singular/plural.
    Picks an "s" suffix for n != 1 (covers 0 and >=2). The
    pluralization is naive; resource labels in this codebase all
    take a simple -s plural (node/nodes, core/cores, GPU/GPUs)."""
    if n == 1:
        return "{0} {1}".format(n, label)
    return "{0} {1}s".format(n, label)


# vi: ts=4 sw=4 expandtab
