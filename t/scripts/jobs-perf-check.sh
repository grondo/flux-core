#!/bin/bash
##############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################
#
# Benchmark and verify flux-jobs(1) output generation using synthetic
# job-list.list responses fed via the hidden --from-stdin option.
#
# This exercises only the Python side of job listing (JobInfo construction
# and output formatting).  It does not model the job-list streaming RPC.
#
# Usage:
#   jobs-perf-check.sh bench [njobs] [iterations]
#      Report the best wall clock time of N runs over njobs jobs.
#
#   jobs-perf-check.sh capture DIR
#      Capture formatted output of several format strings into DIR, for
#      comparison against another revision with diff(1).  Only fields that
#      do not depend on the current time are included for active jobs.
#
set -e

FLUX=${FLUX:-./src/cmd/flux}
GEN=$(dirname "$0")/gen-joblist.py

# All fields, for inactive jobs (no wall clock dependence)
FMT_ALL='{id.f58} {id.dec} {id.hex} {id.words} {userid} {username} {urgency}
{priority} {state} {state_single} {state_emoji} {name} {queue} {cwd} {ntasks}
{ncores} {nnodes} {ranks} {nodelist} {success} {result} {result_abbrev}
{result_emoji} {status} {status_abbrev} {status_emoji} {waitstatus}
{returncode} {t_submit} {t_depend} {t_priority} {t_sched} {t_run} {t_cleanup}
{t_inactive} {runtime} {expiration} {exception.occurred} {exception.severity}
{exception.type} {exception.note} {annotations} {sched} {user}
{sched.reason_pending} {dependencies} {contextual_info} {contextual_time}
{inactive_reason} {duration}'

# Fields that are stable for pending/running jobs (excludes runtime,
# t_remaining, contextual_info/time, which depend on the current time)
FMT_ACTIVE='{id.f58} {userid} {username} {urgency} {priority} {state}
{state_single} {state_emoji} {name} {queue} {cwd} {ntasks} {ncores} {nnodes}
{ranks} {nodelist} {success} {result} {status} {status_abbrev} {status_emoji}
{waitstatus} {returncode} {t_submit} {t_depend} {t_sched} {t_run}
{expiration} {exception.occurred} {annotations} {sched.reason_pending}
{dependencies} {inactive_reason} {duration}'

do_bench() {
    njobs=${1:-40000}
    iters=${2:-5}
    input=$(mktemp)
    trap 'rm -f $input' EXIT
    python3 "$GEN" -n "$njobs" >"$input"
    best=""
    for i in $(seq "$iters"); do
        t=$( { /usr/bin/time -f %e $FLUX jobs --from-stdin -a \
               <"$input" >/dev/null; } 2>&1 )
        best=$(python3 -c "print(min($t, ${best:-$t}))")
    done
    echo "$njobs jobs: best of $iters: ${best}s"
}

do_capture() {
    dir=$1
    test -n "$dir" || { echo "capture: DIR required" >&2; exit 1; }
    mkdir -p "$dir"
    inact=$(mktemp)
    active=$(mktemp)
    trap 'rm -f $inact $active' EXIT
    python3 "$GEN" -n 5000 --inactive-fraction 1.0 >"$inact"
    python3 "$GEN" -n 5000 --inactive-fraction 0.0 >"$active"

    $FLUX jobs --from-stdin -a --format="$FMT_ALL" <"$inact" >"$dir/inactive.out"
    $FLUX jobs --from-stdin -a --json <"$inact" >"$dir/inactive.json"
    $FLUX jobs --from-stdin -a --format="$FMT_ACTIVE" <"$active" >"$dir/active.out"
    rm -f "$dir/builtin.out"
    for fmt in default cute long deps endreason; do
        $FLUX jobs --from-stdin -a --format=$fmt <"$inact" >>"$dir/builtin.out"
    done
    echo "captured output in $dir"
}

cmd=${1:-bench}
shift || true
case "$cmd" in
    bench) do_bench "$@" ;;
    capture) do_capture "$@" ;;
    *) echo "Usage: $0 {bench [njobs] [iters] | capture DIR}" >&2; exit 1 ;;
esac
