#!/bin/sh

test_description='sched-simple amend-r: pool class amends R in a sub-instance'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. $(dirname $0)/sharness.sh

test_under_flux 1 full

ncores=$(flux resource list -no {ncores})

# Print the topology cores in scheduling.children[0] of R on stdin
topo_cores() {
    jq -r "[.scheduling.children[0].topo | .. | .cores? // empty] | join(\",\")"
}

test_expect_success 'create conf with pool-class=TreePool amend-r' '
    cat >amend.toml <<-EOF &&
	[modules]
	alternatives.sched = "sched-simple"
	sched-simple.args = ["pool-class=TreePool", "amend-r"]
	EOF
    cat >job-R.sh <<-EOF &&
	#!/bin/sh
	flux run -n1 true && flux job info \$(flux job last) R
	EOF
    chmod +x job-R.sh
'
test_expect_success 'sub-instance without amend-r cannot use TreePool' '
    cat >noamend.toml <<-EOF &&
	[modules]
	alternatives.sched = "sched-simple"
	sched-simple.args = ["pool-class=TreePool"]
	EOF
    test_must_fail flux alloc -N1 --conf=noamend.toml true 2>noamend.err &&
    test_debug "cat noamend.err" &&
    grep "try amend-r" noamend.err
'
test_expect_success 'exclusive sub-instance amends R with TreePool topology' '
    flux alloc -N1 --conf=amend.toml \
        flux dmesg -H >exclusive.dmesg &&
    test_debug "cat exclusive.dmesg" &&
    grep "pool-class=TreePool" exclusive.dmesg
'
test_expect_success 'job R in exclusive sub-instance carries scheduling' '
    flux alloc -N1 --conf=amend.toml \
        ./job-R.sh >exclusive.R &&
    test_debug "jq -S . exclusive.R" &&
    jq -e ".scheduling.writer == \"TreePool\"" exclusive.R &&
    jq -e ".scheduling.children[0].ranks == \"0\"" exclusive.R
'
test_expect_success 'partial sub-instance topology matches allocated cores' '
    test $ncores -ge 2 &&
    flux alloc -n1 -c2 --conf=amend.toml ./job-R.sh >partial.R &&
    test_debug "jq -S . partial.R" &&
    test "$(topo_cores <partial.R)" = "0-1"
'
test_expect_success 'nested sub-instance replaces inherited scheduling' '
    test $ncores -ge 3 &&
    flux alloc -n1 -c3 --conf=amend.toml \
        flux alloc -n1 -c1 --conf=$(pwd)/amend.toml $(pwd)/job-R.sh \
        >nested.R &&
    test_debug "jq -S . nested.R" &&
    test "$(topo_cores <nested.R)" = "0"
'
test_expect_success 'sub-instance with amend-r and no pool-class fails' '
    cat >nopool.toml <<-EOF &&
	[modules]
	alternatives.sched = "sched-simple"
	sched-simple.args = ["amend-r"]
	EOF
    test_must_fail flux alloc -N1 --conf=nopool.toml true 2>nopool.err &&
    test_debug "cat nopool.err" &&
    grep "pool-class=None does not support amend-r" nopool.err
'
test_expect_success 'amend-r without pool-class fails' '
    test_must_fail flux module reload sched-simple amend-r
'
test_expect_success 'amend-r with non-amending pool-class fails' '
    test_must_fail flux module load sched-simple \
        pool-class=flux.resource.Rv1Pool:Rv1Pool amend-r
'
test_expect_success 'amend-r=false is accepted' '
    flux module load sched-simple amend-r=false
'
test_done
