#!/bin/sh
#
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
#

test_description='Test the flux fake-resources CLI'

. $(dirname $0)/sharness.sh

test_under_flux 1

test_expect_success 'fake-resources: --help succeeds' '
	flux fake-resources --help >help.out &&
	grep "number of fake nodes to inject" help.out
'

test_expect_success 'fake-resources: missing required -N fails' '
	test_must_fail flux fake-resources --cores=2
'

test_expect_success 'fake-resources: cores only' '
	flux fake-resources -N 10 --cores=2 &&
	test $(flux resource list -s free -no "{nnodes}") -eq 10 &&
	test $(flux resource list -s free -no "{ncores}") -eq 20
'

test_expect_success 'fake-resources: with GPUs' '
	flux fake-resources -N 5 --cores=4 --gpus=2 &&
	test $(flux resource list -s free -no "{nnodes}") -eq 5 &&
	test $(flux resource list -s free -no "{ncores}") -eq 20 &&
	test $(flux resource list -s free -no "{ngpus}") -eq 10
'

test_expect_success 'fake-resources: custom host prefix' '
	flux fake-resources -N 3 --cores=2 -H node &&
	test "$(flux resource list -s free -no "{nodelist}")" = "node[0-2]"
'

test_expect_success 'fake-resources: scheduler options pass through' '
	flux fake-resources -N 4 --cores=2 \
		--scheduler-options="queue-depth=16" &&
	test $(flux resource list -s free -no "{nnodes}") -eq 4
'

test_expect_success 'fake-resources: verbose flag emits trace' '
	flux fake-resources -N 4 --cores=2 -v 2>verbose.err &&
	grep -q "Encoding fake R" verbose.err &&
	grep -q "flux module remove" verbose.err &&
	grep -q "flux module load" verbose.err &&
	grep -q "Fake resources injected" verbose.err
'

test_expect_success 'fake-resources: --amend-r with identity plugin' '
	cat >identity-amend.py <<-EOF &&
	def amend(R, fake_resources, hwloc_xml=None):
	    return R
	EOF
	flux fake-resources -N 4 --cores=2 \
		--amend-r=identity-amend.py &&
	test $(flux resource list -s free -no "{nnodes}") -eq 4
'

test_expect_success 'fake-resources: --amend-r missing file fails' '
	test_must_fail flux fake-resources -N 4 \
		--amend-r=/no/such/file 2>missing.err &&
	grep -q "no such file" missing.err
'

test_expect_success 'fake-resources: --amend-r without amend() fails' '
	cat >broken-amend.py <<-EOF &&
	x = 1
	EOF
	test_must_fail flux fake-resources -N 4 \
		--amend-r=broken-amend.py 2>broken.err &&
	grep -q "missing top-level amend" broken.err
'

test_expect_success 'fake-resources: --scheduler swaps in a different module' '
	flux fake-resources -N 4 --cores=2 --scheduler=sched-fifo &&
	flux module list | grep -wq sched-fifo &&
	test_must_fail sh -c "flux module list | grep -wq sched-simple"
'

test_expect_success 'fake-resources: --hwloc-xml uses XML topology' '
	xmlfile=$(find $SHARNESS_TEST_SRCDIR/hwloc-data -name "*.xml" 2>/dev/null \
		| head -1) &&
	test -n "$xmlfile" &&
	flux fake-resources -N 1 --hwloc-xml="$xmlfile" &&
	flux resource list >/dev/null
'

test_expect_success 'fake-resources: idempotent (re-run replaces previous R)' '
	flux fake-resources -N 4 --cores=2 &&
	flux fake-resources -N 8 --cores=4 &&
	test $(flux resource list -s free -no "{nnodes}") -eq 8 &&
	test $(flux resource list -s free -no "{ncores}") -eq 32
'

test_expect_success 'schedbench run: throughput writes results file' '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=20 \
		--tag=test-throughput \
		--results-file=throughput.json &&
	test -f throughput.json &&
	grep -q "\"test_name\": \"throughput\"" throughput.json &&
	grep -q "\"tag\": \"test-throughput\"" throughput.json &&
	grep -q "\"njobs\": 20" throughput.json
'

# The fill-machine sharness sub-cases below cover two resource
# shapes: a cores-only run for the baseline cancel-and-cleanup
# path, and a 1-GPU-per-node variant. The GPU variant catches a
# regression in `flux R encode`'s idset argument formatting that
# previously produced R without GPUs whenever gpus_per_node was
# 1 (the degenerate `-g 0-0` range failed to parse). See the
# IDset usage in fake_resources.build_R_encode_args for the
# fix. t6010 has the in-process GPU allocation regression test.
test_expect_success 'schedbench run: fill-machine writes results file' '
	flux schedbench run fill-machine \
		-N 2 --cores-per-node=2 \
		--gpus-per-node=0 \
		--slot-cores=1 \
		--results-file=fillmachine.json &&
	test -f fillmachine.json &&
	grep -q "\"test_name\": \"fill-machine\"" fillmachine.json &&
	grep -q "\"njobs\": 4" fillmachine.json
'

test_expect_success 'schedbench run: fill-machine with 1 GPU/node' '
	flux schedbench run fill-machine \
		-N 2 --cores-per-node=2 \
		--gpus-per-node=1 \
		--slot-cores=1 --slot-gpus=1 \
		--results-file=fillmachine-gpu.json &&
	test -f fillmachine-gpu.json &&
	grep -q "\"test_name\": \"fill-machine\"" fillmachine-gpu.json &&
	grep -q "\"njobs\": 2" fillmachine-gpu.json
'

test_expect_success 'schedbench run --no-save skips results file' '
	rm -f no-save.json &&
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=10 --no-save --results-file=no-save.json &&
	test ! -e no-save.json
'

# --exec sub-cases. Each `flux schedbench run --exec` runs
# under its own `flux start -s 1` so the broker is fresh:
# real hwloc-detected cores on the local host and a stock
# sched-simple loaded by the standard rc1. The outer
# test_under_flux broker is unsuitable because earlier tests
# call `fake.install`, which swaps in synthetic resources
# keyed on nonexistent hosts (`fake0`, `fake1`, ...) — fine
# for mock mode (the broker never invokes flux-shell) but
# fatal under `--exec`, where flux-shell can't launch on
# hosts that aren't there.
#
# The argparse-validation case doesn't connect to a broker
# (it exits during parse) and the report case is pure JSON
# rendering, so those run in the outer broker without
# wrapping.
test_expect_success 'schedbench run without -N fails unless --exec' '
	test_must_fail flux schedbench run throughput \
		--njobs=4 --no-save 2>noN.err &&
	grep -q "required" noN.err
'

test_expect_success 'schedbench run throughput --exec succeeds' '
	flux start -s 1 \
		flux schedbench run throughput --exec \
			--njobs=4 --no-save --ui=off >exec-thr.out &&
	grep -q "\"name\": \"result\"" exec-thr.out &&
	grep -q "\"real_exec\":" exec-thr.out
'

test_expect_success 'schedbench run throughput --exec records real_exec=true' '
	rm -f exec.json &&
	flux start -s 1 \
		flux schedbench run throughput --exec \
			--njobs=4 --results-file=exec.json --ui=off >/dev/null &&
	grep -q "\"real_exec\": true" exec.json
'

test_expect_success 'schedbench run fill-machine --exec succeeds' '
	flux start -s 1 \
		flux schedbench run fill-machine --exec \
			--no-save --ui=off >exec-fm.out &&
	grep -q "\"name\": \"result\"" exec-fm.out &&
	grep -q "time_to_fill" exec-fm.out
'

test_expect_success 'schedbench report renders REAL column' '
	flux schedbench report throughput \
		--results-file=exec.json >exec-report.out &&
	head -1 exec-report.out | grep -q "REAL"
'

# UI-mode coverage. Sharness redirects stdout to a file so
# isatty() is false during these tests; --ui=auto correctly
# falls back to JSON. --ui=off makes the fallback explicit
# (covers the path users will take when piping to jq), and
# --ui=on forces the terminal renderer even on a non-TTY so
# we exercise the rendering path itself. Color is disabled to
# keep the substring grep robust against escape sequences.
test_expect_success 'schedbench run --ui=off emits JSON event stream' '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=4 --no-save \
		--ui=off >ui-off.out &&
	grep -q "\"name\": \"test.start\"" ui-off.out &&
	grep -q "\"name\": \"test.complete\"" ui-off.out
'

test_expect_success 'schedbench run --ui=on renders terminal block' '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=4 --no-save \
		--ui=on --color=never >ui-on.out &&
	grep -q "flux schedbench" ui-on.out &&
	grep -q "throughput" ui-on.out &&
	grep -q "elapsed" ui-on.out
'

test_expect_success "schedbench run --quiet --ui=on shows results without progress" '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=4 --no-save \
		--ui=on --quiet >ui-quiet.out 2>ui-quiet.err &&
	grep -q "flux schedbench" ui-quiet.out &&
	grep -q "throughput" ui-quiet.out &&
	! grep -q "█" ui-quiet.out
'

test_expect_success "schedbench run --quiet suppresses INFO log messages" '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=4 --no-save --quiet \
		>quiet-info.out 2>quiet-info.err &&
	! grep -q "^flux-schedbench: INFO:" quiet-info.err &&
	! grep -q "^flux-schedbench: INFO:" quiet-info.out
'

test_expect_success "schedbench run --quiet non-TTY emits single JSON object" '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=4 --no-save --quiet --ui=off \
		>quiet-json.out 2>&1 &&
	test "$(wc -l <quiet-json.out)" -eq 1 &&
	grep -q "\"throughput\":" quiet-json.out &&
	python3 -c "import json,sys; json.loads(open(\"quiet-json.out\").read())"
'

test_expect_success 'schedbench report TEST prints a table header' '
	flux schedbench report throughput \
		--results-file=throughput.json >report.out &&
	head -1 report.out | grep -q "SCHED" &&
	head -1 report.out | grep -q "JOBS"
'

test_expect_success "schedbench report TEST prints headline metric column" '
	flux schedbench report throughput \
		--results-file=throughput.json >report.out &&
	head -1 report.out | grep -q "THRPUT"
'

test_expect_success "schedbench report -o long includes extra columns" '
	flux schedbench report throughput -o long \
		--results-file=throughput.json >long.out &&
	head -1 long.out | grep -q "ALLOC"
'

test_expect_success "schedbench report -o csv emits comma-separated output" '
	flux schedbench report throughput -o csv \
		--results-file=throughput.json >csv.out &&
	head -1 csv.out | grep -q "," &&
	head -1 csv.out | grep -q "SCHED"
'

test_expect_success "schedbench report --no-header skips header row" '
	flux schedbench report throughput --no-header \
		--results-file=throughput.json >noheader.out &&
	! head -1 noheader.out | grep -q "SCHED"
'

test_expect_success "schedbench report --filter narrows to matching runs" '
	flux schedbench report throughput \
		--results-file=throughput.json \
		--filter=scheduler.name=sched-simple \
		>match.out 2>&1 || true &&
	test_must_fail flux schedbench report throughput \
		--results-file=throughput.json \
		--filter=tag=nonexistent-tag >no-match.out 2>&1 &&
	grep -q "no .* match" no-match.out
'

test_expect_success "schedbench report missing file is an error" '
	test_must_fail flux schedbench report throughput \
		--results-file=does-not-exist.json
'

test_expect_success "schedbench report requires a TEST argument" '
	test_must_fail flux schedbench report \
		--results-file=throughput.json 2>noarg.out &&
	grep -q "required" noarg.out
'

test_expect_success 'schedbench run: invalid benchmark name fails' '
	test_must_fail flux schedbench run no-such-benchmark -N 4
'

test_expect_success 'schedbench: missing required -N fails' '
	test_must_fail flux schedbench run throughput
'

test_expect_success 'schedbench run --watcher=per-job runs and records watcher' '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--watcher=per-job --njobs=20 \
		--results-file=perjob.json &&
	grep -q "\"watcher\": \"per-job\"" perjob.json
'

test_expect_success 'schedbench run --watcher=journal is the default' '
	flux schedbench run throughput \
		-N 4 --cores-per-node=2 \
		--gpus-per-node=0 \
		--njobs=20 \
		--results-file=journal.json &&
	grep -q "\"watcher\": \"journal\"" journal.json
'

test_expect_success 'schedbench run --watcher=bogus rejected at argparse' '
	test_must_fail flux schedbench run throughput \
		-N 4 --watcher=bogus
'

test_done
