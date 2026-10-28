#!/bin/sh

test_description='Test runtime queues with exclusive properties'

. `dirname $0`/sharness.sh

# Fake resources give four distinct hostnames on a single broker, so a
# hostlist resolves to a subset of ranks.  They are installed before the
# resource module loads, so the donor queue's membership is derived from
# its configured 'hosts' at startup, as it would be in a real instance.
test_under_flux 1 full \
    --conf=fake-resources.nnodes=4 \
    --conf=fake-resources.cores-per-node=2 \
    --conf=fake-resources.host-prefix=node \
    --conf=queues.batch.hosts=all \
    --conf=policy.jobspec.defaults.system.queue=batch

# Jobs run under the test execution implementation, since there are no
# real resources behind the fake ones.
RUN="--setattr=system.exec.test.run_duration=0.01s"

# Return the nodelist of a completed job.
job_nodes() {
	flux jobs -no "{nodelist}" $1
}

test_expect_success 'the donor queue covers all nodes' '
	flux queue start --all &&
	test "$(flux resource list -s all -no "{nodelist}" -q batch)" = "node[0-3]"
'

test_expect_success 'flux queue add creates a queue' '
	flux queue add dat targets=node[2-3]
'

test_expect_success 'the new queue is listed' '
	flux queue list -no "{queue}" >qlist.out &&
	test_debug "cat qlist.out" &&
	grep -x dat qlist.out
'

test_expect_success 'the new queue has its targets' '
	test_debug "flux resource list -s all -o \"{queue} {nodelist}\"" &&
	test "$(flux resource list -s all -no "{nodelist}" -q dat)" = "node[2-3]"
'

# A queue is created enabled and stopped, so a user may submit to it
# before it starts.  This also proves job-ingest picked up the queue
# without a config reload.
test_expect_success 'a job may be submitted to the new queue' '
	id1=$(flux submit -q dat -N2 $RUN true) &&
	echo $id1 >id1 &&
	flux job wait-event -t 30 $id1 depend
'

test_expect_success 'the job stays in SCHED while the queue is stopped' '
	test_must_fail flux job wait-event -t 2 $(cat id1) alloc
'

test_expect_success 'flux queue start runs the job on the queue targets' '
	flux queue start dat &&
	flux job status -v $(cat id1) &&
	test "$(job_nodes $(cat id1))" = "node[2-3]"
'

# Rule 2: the donor queue's jobs may no longer use the exclusive hosts.
test_expect_success 'a donor job avoids the exclusive hosts' '
	id2=$(flux submit -q batch -N2 --setattr=system.exec.test.run_duration=300s true) &&
	echo $id2 >id2 &&
	flux job wait-event -t 30 $id2 alloc &&
	test "$(job_nodes $id2)" = "node[0-1]"
'

# Rule 1: feasibility ignores the exclusive set, so a job that needs the
# whole instance waits rather than being rejected.  This is the behavior
# the design exists to get.
test_expect_success 'a donor job that needs all nodes is pending, not rejected' '
	id3=$(flux submit -q batch -N4 $RUN true) &&
	echo $id3 >id3 &&
	test_must_fail flux job wait-event -t 2 $id3 alloc &&
	test_must_fail flux job wait-event -t 0.1 $id3 exception
'

test_expect_success 'flux queue stop returns the hosts' '
	flux queue stop dat &&
	flux cancel $(cat id2) &&
	flux job status -v $(cat id3) &&
	test "$(job_nodes $(cat id3))" = "node[0-3]"
'

test_expect_success 'flux queue remove removes the queue' '
	flux queue remove dat &&
	flux queue list -no "{queue}" >qlist2.out &&
	test_debug "cat qlist2.out" &&
	test_must_fail grep -x dat qlist2.out
'

# N.B. {properties} rather than {propertiesx}: the latter deliberately
# hides queue membership properties, so it cannot tell the difference.
test_expect_success 'the membership property is gone' '
	flux resource list -s all -no "{properties}" >gone.out &&
	test_debug "cat gone.out" &&
	grep "+batch" gone.out &&
	test_must_fail grep "+dat" gone.out
'

test_expect_success 'a job may no longer be submitted to the queue' '
	test_must_fail flux submit -q dat -N1 $RUN true
'

test_expect_success 'adding an existing queue name fails' '
	test_must_fail flux queue add batch targets=node0 2>exists.err &&
	test_debug "cat exists.err" &&
	grep -i "already exists" exists.err
'

test_expect_success 'an invalid queue name fails' '
	test_must_fail flux queue add "a b" targets=node0 2>name.err &&
	test_debug "cat name.err" &&
	grep -i "property string" name.err
'

test_expect_success 'an unknown host fails' '
	test_must_fail flux queue add dat2 targets=nosuchhost 2>host.err &&
	test_debug "cat host.err"
'

test_expect_success 'a failed add leaves no queue behind' '
	flux queue list -no "{queue}" >qlist3.out &&
	test_must_fail grep -x dat2 qlist3.out
'

test_expect_success 'targets is required' '
	test_must_fail flux queue add dat3 foo=bar 2>opt.err &&
	test_debug "cat opt.err" &&
	grep -i "unsupported option" opt.err
'

# targets accepts an RFC 22 idset of ranks as well as a hostlist, which
# is what makes it possible to exercise runtime queues on an instance
# whose ranks all share one hostname.
test_expect_success 'targets may be an idset of ranks' '
	flux queue add idq targets=2-3 &&
	test "$(flux resource list -s all -no "{nodelist}" -q idq)" = "node[2-3]" &&
	flux queue remove idq
'

test_expect_success 'an out of range rank fails' '
	test_must_fail flux queue add badrank targets=99 2>rank.err &&
	test_debug "cat rank.err"
'

test_expect_success 'removing a configured queue fails' '
	test_must_fail flux queue remove batch 2>conf.err &&
	test_debug "cat conf.err" &&
	grep -i "runtime queue" conf.err
'

test_expect_success 'removing a nonexistent queue fails' '
	test_must_fail flux queue remove nosuchqueue
'

test_expect_success 'removing a queue with a pending job fails' '
	flux queue add dat4 targets=node[2-3] &&
	id4=$(flux submit -q dat4 -N1 $RUN true) &&
	test_must_fail flux queue remove dat4 2>busy.err &&
	test_debug "cat busy.err" &&
	grep -i "pending jobs" busy.err &&
	flux cancel $id4 &&
	flux job wait-event -t 30 $id4 clean &&
	flux queue remove dat4
'

# A start or stop that spans an exclusive queue would need one update
# per queue, which is out of scope for now.
test_expect_success 'queue start --all fails with an exclusive queue' '
	flux queue add dat5 targets=node[2-3] &&
	test_must_fail flux queue start --all 2>all.err &&
	test_debug "cat all.err" &&
	grep -i "not supported with exclusive queues" all.err &&
	flux queue remove dat5
'

test_done
