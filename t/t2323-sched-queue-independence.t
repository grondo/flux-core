#!/bin/sh

test_description='Test that one blocked queue does not block the others'

. `dirname $0`/sharness.sh

# Fake resources give four distinct hostnames on a single broker, so a
# configured queue's hostlist selects a subset of ranks.
test_under_flux 1 full \
    --conf=fake-resources.nnodes=4 \
    --conf=fake-resources.cores-per-node=2 \
    --conf=fake-resources.host-prefix=node \
    --conf=queues.batch.hosts=node[0-1] \
    --conf=queues.other.hosts=node[2-3] \
    --conf=policy.jobspec.defaults.system.queue=batch

# Jobs run under the test execution implementation, since there are no
# real resources behind the fake ones.
LONG="--flags=debug --setattr=system.exec.test.run_duration=60s"
SHORT="--setattr=system.exec.test.run_duration=0.01s"

test_expect_success 'start both queues' '
	flux queue start --all
'

#
# Two configured queues on disjoint nodes.  A job that cannot be
# allocated in one queue must not keep the other queue from running.
#
test_expect_success 'occupy the batch nodes' '
	id=$(flux submit -q batch -N2 $LONG true) &&
	echo $id >occupy.id &&
	flux job wait-event -t 30 $id start
'

test_expect_success 'a second batch job is blocked' '
	id=$(flux submit -q batch -N2 $SHORT true) &&
	echo $id >blocked.id &&
	test_must_fail flux job wait-event -t 2 $id alloc
'

test_expect_success 'a job in the other queue still runs' '
	id=$(flux submit -q other -N2 $SHORT true) &&
	flux job wait-event -t 30 $id alloc &&
	flux job wait-event -t 30 $id clean
'

# Within one queue the order is still strict FIFO: a job must never
# start before one ahead of it in the same queue.
test_expect_success 'a job behind the blocked one in its own queue waits' '
	id=$(flux submit -q batch -N1 $SHORT true) &&
	test_must_fail flux job wait-event -t 2 $id alloc &&
	flux cancel $id &&
	flux job wait-event -t 30 $id clean
'

test_expect_success 'the blocked batch job runs once its nodes are free' '
	flux cancel $(cat occupy.id) &&
	flux job wait-event -t 30 $(cat occupy.id) clean &&
	flux job wait-event -t 30 $(cat blocked.id) clean
'

#
# The same check with a runtime queue, whose nodes are set aside by the
# exclusive set rather than by a configured hostlist.  This is the case
# the feature exists for: an admin reserves nodes for a group, and a
# pending job in the donor queue must not keep that group from using
# them.
#
test_expect_success 'reconfigure with a single donor queue over all nodes' '
	flux config load <<-EOT &&
	[queues.batch]
	hosts = "all"
	[policy.jobspec.defaults.system]
	queue = "batch"
	EOT
	flux module remove sched-simple &&
	flux module reload resource monitor-force-up noverify &&
	flux module load sched-simple &&
	flux queue start batch
'

test_expect_success 'create and start a runtime queue' '
	flux queue add dat targets=node[2-3] &&
	flux queue start dat
'

test_expect_success 'a donor job that needs every node is blocked' '
	id=$(flux submit -q batch -N4 $SHORT true) &&
	echo $id >donor.id &&
	test_must_fail flux job wait-event -t 2 $id alloc &&
	test_must_fail flux job wait-event -t 0.1 $id exception
'

test_expect_success 'the runtime queue still runs its own jobs' '
	id=$(flux submit -q dat -N2 $SHORT true) &&
	flux job wait-event -t 30 $id alloc &&
	flux job wait-event -t 30 $id clean
'

test_expect_success 'the donor job runs once the runtime queue stops' '
	flux queue stop dat &&
	flux job wait-event -t 30 $(cat donor.id) clean
'

test_done
