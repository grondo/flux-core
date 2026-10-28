#!/bin/sh

test_description='Test exclusive property set'

. `dirname $0`/sharness.sh

SIZE=4
test_under_flux $SIZE job

RPC=${FLUX_BUILD_DIR}/t/request/rpc

# Usage: property_update JSON
#   Send a resource.property-update request with the given payload.
property_update() {
	echo "$1" | $RPC resource.property-update
}

# Return the scheduler's exclusive property set as a JSON array.
sched_exclusive() {
	flux python -c \
"import flux, json; print(json.dumps(flux.Flux().rpc(\"sched.resource-status\").get()[\"exclusive\"]))"
}

test_expect_success 'the exclusive set is initially empty' '
	echo "[]" >initial.exp &&
	sched_exclusive >initial.out &&
	test_cmp initial.exp initial.out
'

test_expect_success 'exclusive-add adds a name' '
	property_update "{\"property-add\":{\"+dat\":\"2-3\"},
	                  \"exclusive-add\":[\"+dat\"]}" &&
	echo "[\"+dat\"]" >add.exp &&
	sched_exclusive >add.out &&
	test_cmp add.exp add.out
'

test_expect_success 'a property-only update leaves the set alone' '
	property_update "{\"property-add\":{\"+other\":\"0\"}}" &&
	sched_exclusive >other.out &&
	test_cmp add.exp other.out
'

# Rule 2: an exclusive rank is allocated only to a job in the queue
# that owns it.  There are no named queues here, so no job is a member
# and every rank in E is set aside -- including from a job that names
# the property, which is the access rule.
test_expect_success 'a job avoids the exclusive ranks' '
	rank=$(flux run -N1 flux getattr rank) &&
	test_debug "echo job ran on rank $rank" &&
	test "$rank" -ge 0 && test "$rank" -le 1
'

test_expect_success 'naming the property does not grant access' '
	id=$(flux submit -N1 --requires=+dat true) &&
	test_must_fail flux job wait-event -t 2 $id alloc &&
	test_must_fail flux job wait-event -t 0.1 $id exception &&
	flux cancel $id &&
	flux job wait-event -t 30 $id clean
'

# Rule 1: feasibility ignores E, so this job waits instead of being
# rejected.  This is the behavior the whole design exists to get.
test_expect_success 'a job that cannot fit outside E is pending, not rejected' '
	id=$(flux submit -N4 true) &&
	test_must_fail flux job wait-event -t 2 $id alloc &&
	test_must_fail flux job wait-event -t 0.1 $id exception &&
	flux cancel $id
'

test_expect_success 'exclusive-remove removes a name' '
	property_update "{\"exclusive-remove\":[\"+dat\"]}" &&
	echo "[]" >rm.exp &&
	sched_exclusive >rm.out &&
	test_cmp rm.exp rm.out
'

test_expect_success 'the ranks are available again' '
	flux run -N4 true
'

test_expect_success 'a name without the + prefix is rejected' '
	test_must_fail property_update "{\"exclusive-add\":[\"dat\"]}" \
	    2>plus.err &&
	test_debug "cat plus.err" &&
	grep "instance-local" plus.err
'

test_expect_success 'a non-array exclusive-add is rejected' '
	test_must_fail property_update "{\"exclusive-add\":\"+dat\"}"
'

test_expect_success 'a non-string name is rejected' '
	test_must_fail property_update "{\"exclusive-add\":[42]}"
'

test_expect_success 'a rejected update leaves the set unchanged' '
	property_update "{\"exclusive-add\":[\"+dat\"]}" &&
	test_must_fail property_update "{\"exclusive-add\":[\"bad\"]}" &&
	echo "[\"+dat\"]" >unchanged.exp &&
	sched_exclusive >unchanged.out &&
	test_cmp unchanged.exp unchanged.out
'

test_expect_success 'the set is deduplicated and sorted' '
	property_update "{\"exclusive-add\":[\"+zz\",\"+aa\",\"+dat\"]}" &&
	echo "[\"+aa\", \"+dat\", \"+zz\"]" >sorted.exp &&
	sched_exclusive >sorted.out &&
	test_cmp sorted.exp sorted.out &&
	property_update "{\"exclusive-remove\":[\"+zz\",\"+aa\"]}"
'

# The set is sent in the first acquire response, so a scheduler that
# reloads while a queue is started picks it up without being told.
test_expect_success 'a reloaded scheduler acquires the exclusive set' '
	flux module reload sched-simple &&
	echo "[\"+dat\"]" >reload.exp &&
	sched_exclusive >reload.out &&
	test_cmp reload.exp reload.out
'

test_expect_success 'the set does not survive a resource reload' '
	flux module remove sched-simple &&
	flux module reload resource &&
	flux module load sched-simple &&
	echo "[]" >rreload.exp &&
	sched_exclusive >rreload.out &&
	test_cmp rreload.exp rreload.out
'

test_done
