#!/bin/sh

test_description='Test runtime property update'

. `dirname $0`/sharness.sh

SIZE=4
test_under_flux $SIZE job

RPC=${FLUX_BUILD_DIR}/t/request/rpc

# Usage: property_update JSON
#   Send a resource.property-update request with the given payload.
property_update() {
	echo "$1" | $RPC resource.property-update
}

# Return the properties object of the R served by the resource module,
# or "null" if there are none.
served_properties() {
	flux python -c \
"import flux, json; print(json.dumps(flux.Flux().rpc(\"resource.get\",{}).get()[\"R\"][\"execution\"].get(\"properties\"), sort_keys=True))"
}

# Return the properties object of the scheduler's resource set.
sched_properties() {
	flux python -c \
"import flux, json; print(json.dumps(flux.Flux().rpc(\"sched.resource-status\").get()[\"all\"][\"execution\"].get(\"properties\"), sort_keys=True))"
}

# Return the properties object of R as committed to the KVS.
stored_properties() {
	flux kvs get resource.R \
	    | jq -S -c '.execution.properties // null'
}

# Seed resource.R with distinct fake hostnames so that a hostlist target
# resolves to a subset of ranks.  Every rank has the same real hostname
# under test_under_flux.
test_expect_success 'seed resource.R with named hosts' '
	flux module remove sched-simple &&
	flux R encode -r 0-3 -H test[0-3] -c 0-1 \
	    | flux kvs put -r resource.R=- &&
	flux module reload resource monitor-force-up &&
	flux module load sched-simple
'

test_expect_success 'resource module has no properties initially' '
	echo null >initial.exp &&
	served_properties >initial.out &&
	test_cmp initial.exp initial.out
'

test_expect_success 'property-update adds a property' '
	property_update "{\"property-add\":{\"+batch\":\"0-1\"}}" &&
	echo "{\"+batch\": \"0-1\"}" >add.exp &&
	served_properties >add.out &&
	test_cmp add.exp add.out
'

test_expect_success 'the scheduler applied the property too' '
	sched_properties >sadd.out &&
	test_cmp add.exp sadd.out
'

# This is what the RPC exists to guarantee: the scheduler has applied the
# property, so a job that names it is satisfiable and lands on those ranks.
test_expect_success 'a job requiring the property lands on its ranks' '
	rank=$(flux run -N1 --requires=+batch flux getattr rank) &&
	test_debug "echo job ran on rank $rank" &&
	test "$rank" -ge 0 && test "$rank" -le 1
'

test_expect_success 'targets may be a hostlist' '
	property_update "{\"property-add\":{\"+hq\":\"test[2-3]\"}}" &&
	served_properties >hq.out &&
	jq -e -r ".\"+hq\"" <hq.out >hq_ranks.out &&
	echo "2-3" >hq_ranks.exp &&
	test_cmp hq_ranks.exp hq_ranks.out
'

test_expect_success 'property-update removes a property' '
	property_update "{\"property-remove\":{\"+hq\":\"2-3\"}}" &&
	served_properties >rm.out &&
	test_must_fail jq -e -r ".\"+hq\"" <rm.out
'

test_expect_success 'a job requiring a removed property is unsatisfiable' '
	property_update "{\"property-remove\":{\"+batch\":\"0-1\"}}" &&
	test_must_fail flux run -N1 --requires=+batch true 2>unsat.err &&
	test_debug "cat unsat.err" &&
	grep -i unsatisfiable unsat.err
'

# Both key sets are applied as one update, which is what starting a
# runtime queue over a donor queue requires.
test_expect_success 'add and remove are applied as a single update' '
	property_update "{\"property-add\":{\"+donor\":\"0-3\"}}" &&
	property_update "{\"property-add\":{\"+new\":\"2-3\"},
	                  \"property-remove\":{\"+donor\":\"2-3\"}}" &&
	echo "{\"+donor\": \"0-1\", \"+new\": \"2-3\"}" >move.exp &&
	served_properties >move.out &&
	test_cmp move.exp move.out
'

test_expect_success 'an update that fails leaves R unchanged' '
	served_properties >before.out &&
	test_must_fail property_update "{\"property-add\":{\"+bad\":\"99\"},
	                                 \"property-remove\":{\"+donor\":\"0-1\"}}" &&
	served_properties >after.out &&
	test_cmp before.out after.out
'

test_expect_success 'an unknown host fails' '
	test_must_fail property_update \
	    "{\"property-add\":{\"+x\":\"nosuchhost\"}}" 2>host.err &&
	test_debug "cat host.err"
'

test_expect_success 'an invalid property name fails' '
	test_must_fail property_update "{\"property-add\":{\"+a^b\":\"0\"}}"
'

# Only instance-local properties may be set at runtime: a bare property
# describes the hardware, belongs in [resource.config], and would have no
# authority to restore it after a module reload.
test_expect_success 'a property without the + prefix is rejected' '
	test_must_fail property_update "{\"property-add\":{\"gpu\":\"0\"}}" \
	    2>plus.err &&
	test_debug "cat plus.err" &&
	grep "instance-local" plus.err
'

test_expect_success 'runtime properties are not committed to the KVS' '
	echo null >stored.exp &&
	stored_properties >stored.out &&
	test_cmp stored.exp stored.out
'

# The sched-status cache is built from the R the module serves, so it
# must be invalidated when a runtime property changes, else a queue
# added at runtime is invisible to flux-resource.
test_expect_success 'flux resource list sees a runtime property' '
	property_update "{\"property-add\":{\"+rl\":\"0-1\"}}" &&
	flux resource list -s all -no "{propertiesx}" >rl.out &&
	test_debug "cat rl.out" &&
	grep "+rl" rl.out &&
	property_update "{\"property-remove\":{\"+rl\":\"0-1\"}}" &&
	flux resource list -s all -no "{propertiesx}" >rl2.out &&
	test_must_fail grep "+rl" rl2.out
'

test_expect_success 'a property-update event is posted to the journal' '
	flux resource eventlog >eventlog.out &&
	test_debug "cat eventlog.out" &&
	grep property-update eventlog.out
'

test_expect_success 'but not to the KVS eventlog' '
	flux kvs get resource.eventlog >kvslog.out &&
	test_must_fail grep property-update kvslog.out
'

# With no scheduler there is nothing to desynchronize from, so the update
# succeeds and the next acquire serves the updated R.  Otherwise a runtime
# queue operation would fail whenever the scheduler happened to be
# reloading.
test_expect_success 'property-update works with no scheduler loaded' '
	flux module remove sched-simple &&
	property_update "{\"property-add\":{\"+nosched\":\"0-1\"}}" &&
	served_properties >nosched.out &&
	jq -e -r ".\"+nosched\"" <nosched.out
'

test_expect_success 'a reloaded scheduler acquires the updated R' '
	flux module load sched-simple &&
	sched_properties >reacquire.out &&
	jq -e -r ".\"+nosched\"" <reacquire.out
'

# The properties are derived state: a reload rebuilds R from its source,
# and the job-manager is the authority that re-pushes runtime membership.
test_expect_success 'runtime properties do not survive a resource reload' '
	flux module remove sched-simple &&
	flux module reload resource &&
	flux module load sched-simple &&
	echo null >reload.exp &&
	served_properties >reload.out &&
	test_cmp reload.exp reload.out
'

test_done
