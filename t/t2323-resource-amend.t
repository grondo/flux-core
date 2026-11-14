#!/bin/sh

test_description='Test resource.amend RPC and sched.amend-r config'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. $(dirname $0)/sharness.sh

test_under_flux 2 full

ncores=$(flux resource list -no {ncores})

# Usage: amend_onrank RANK JSON
amend_onrank() {
	flux python -c "import flux,json; print(flux.Flux().rpc(\"resource.amend\",json.loads('$2'),nodeid=$1).get())"
}
# Usage: amend JSON
amend() {
	amend_onrank 0 "$1"
}
# Print the cores in R.scheduling.children[0] of R on stdin
topo_cores() {
	jq -r "[.scheduling.children[0].topo | .. | .cores? // empty] | join(\",\")"
}

test_expect_success 'resource.amend fails on rank > 0' '
	test_must_fail amend_onrank 1 "{\"scheduling\": {}}" 2>rank.err &&
	test_debug "cat rank.err" &&
	grep "only available on rank 0" rank.err
'
test_expect_success 'resource.amend fails with bad payload' '
	test_must_fail amend "{}" 2>payload.err &&
	test_debug "cat payload.err" &&
	grep "Protocol error" payload.err
'
test_expect_success 'resource.amend fails with non-object scheduling key' '
	test_must_fail amend "{\"scheduling\": 1}" 2>type.err &&
	test_debug "cat type.err" &&
	grep "must be an object" type.err
'
test_expect_success 'resource.amend fails while scheduler holds resources' '
	test_must_fail amend "{\"scheduling\": {}}" 2>busy.err &&
	test_debug "cat busy.err" &&
	grep "resources are busy" busy.err
'
test_expect_success 'resource.amend works with scheduler unloaded' '
	flux module remove sched-simple &&
	amend "{\"scheduling\": {\"writer\": \"test\", \"x\": 1}}" &&
	flux kvs get resource.R | jq -e ".scheduling == {\"writer\": \"test\", \"x\": 1}"
'
test_expect_success 'resource.amend replaces existing scheduling key' '
	amend "{\"scheduling\": {\"y\": 2}}" &&
	flux kvs get resource.R | jq -e ".scheduling == {\"y\": 2}"
'
test_expect_success 'amended R persists across resource module reload' '
	flux module reload resource &&
	flux kvs get resource.R | jq -e ".scheduling == {\"y\": 2}"
'
test_expect_success 'amended R is acquired by the scheduler' '
	flux python -c "
import flux, json
from flux.constants import FLUX_RPC_STREAMING
f = flux.Flux().rpc(\"resource.acquire\", flags=FLUX_RPC_STREAMING)
print(json.dumps(f.get()[\"resources\"]))
" >acquired.R &&
	jq -e ".scheduling == {\"y\": 2}" acquired.R
'
test_expect_success 'clear scheduling key and reload sched-simple' '
	flux kvs put resource.R="$(flux kvs get resource.R | jq -c "del(.scheduling)")" &&
	flux module reload resource &&
	flux module load sched-simple
'
test_expect_success 'create amend-r configs' '
	cat >amend.toml <<-EOF &&
	[sched]
	module = "sched-simple"
	amend-r = "TreePool"
	EOF
	cat >bad-amend.toml <<-EOF &&
	[sched]
	module = "sched-simple"
	amend-r = "NoSuchAmender"
	EOF
	cat >job-R.sh <<-EOF &&
	#!/bin/sh
	flux run -n1 true && flux job info \$(flux job last) R
	EOF
	chmod +x job-R.sh
'
test_expect_success 'amend-r=TreePool amends R in an exclusive alloc instance' '
	flux alloc -N1 --conf=amend.toml \
	    flux kvs get resource.R >exclusive.R &&
	test_debug "jq -S . exclusive.R" &&
	jq -e ".scheduling.writer == \"TreePool\"" exclusive.R &&
	jq -e ".scheduling.children[0].ranks == \"0\"" exclusive.R
'
test_expect_success 'sched-simple selects TreePool from the amended R' '
	flux alloc -N1 --conf=amend.toml flux dmesg -H >exclusive.dmesg &&
	test_debug "cat exclusive.dmesg" &&
	grep "pool-class=TreePool" exclusive.dmesg
'
test_expect_success 'job R in alloc instance carries TreePool scheduling' '
	flux alloc -N1 --conf=amend.toml ./job-R.sh >job.R &&
	test_debug "jq -S . job.R" &&
	jq -e ".scheduling.writer == \"TreePool\"" job.R
'
test_expect_success 'partial alloc instance topology matches allocated cores' '
	test $ncores -ge 2 &&
	flux alloc -n1 -c2 --conf=amend.toml \
	    flux kvs get resource.R >partial.R &&
	test_debug "jq -S . partial.R" &&
	test "$(topo_cores <partial.R)" = "0-1"
'
test_expect_success 'nested alloc instance replaces inherited scheduling key' '
	test $ncores -ge 3 &&
	flux alloc -n1 -c3 --conf=amend.toml \
	    flux alloc -n1 -c1 --conf=$(pwd)/amend.toml \
	        flux kvs get resource.R >nested.R &&
	test_debug "jq -S . nested.R" &&
	test "$(topo_cores <nested.R)" = "0"
'
test_expect_success 'amend-r with unknown name fails instance startup' '
	test_must_fail flux alloc -N1 --conf=bad-amend.toml true 2>bad.err &&
	test_debug "cat bad.err" &&
	grep "flux.resource.NoSuchAmender" bad.err
'
test_done
