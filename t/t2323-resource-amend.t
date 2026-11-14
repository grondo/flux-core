#!/bin/sh

test_description='Test resource.amend RPC'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. $(dirname $0)/sharness.sh

test_under_flux 2 full

# Usage: amend_onrank RANK JSON
amend_onrank() {
	flux python -c "import flux,json; print(flux.Flux().rpc(\"resource.amend\",json.loads('$2'),nodeid=$1).get())"
}
# Usage: amend JSON
amend() {
	amend_onrank 0 "$1"
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
test_done
