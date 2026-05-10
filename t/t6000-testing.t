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

test_done
