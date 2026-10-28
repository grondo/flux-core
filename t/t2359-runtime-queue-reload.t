#!/bin/sh

test_description='Test runtime queues across module and config reloads'

. `dirname $0`/sharness.sh

# Fake resources give four distinct hostnames on a single broker, so a
# hostlist resolves to a subset of ranks.  See t2358.
test_under_flux 1 full \
    --conf=fake-resources.nnodes=4 \
    --conf=fake-resources.cores-per-node=2 \
    --conf=fake-resources.host-prefix=node \
    --conf=queues.batch.hosts=all \
    --conf=policy.jobspec.defaults.system.queue=batch

# Jobs run under the test execution implementation, since there are no
# real resources behind the fake ones.  A long duration keeps a job
# running across a reload.
LONG="--flags=debug --setattr=system.exec.test.run_duration=100s"
SHORT="--setattr=system.exec.test.run_duration=0.01s"

nodelist() {
	flux jobs -no "{nodelist}" $1
}

# The properties object of the R served by the resource module.
served_props() {
	flux python -c \
"import flux, json; print(json.dumps(flux.Flux().rpc(\"resource.get\",{}).get()[\"R\"][\"execution\"].get(\"properties\"), sort_keys=True))"
}

# The exclusive property set (E) held by the scheduler.
sched_exclusive() {
	flux python -c \
"import flux, json; print(json.dumps(sorted(flux.Flux().rpc(\"sched.resource-status\").get()[\"exclusive\"])))"
}

key_get() {
	flux kvs get queues.runtime 2>/dev/null
}

queue_state() {
	flux queue list -no "{queue} {enabled} {started}" | grep "^$1 "
}

test_expect_success 'the donor queue covers all nodes' '
	flux queue start --all &&
	test "$(flux resource list -s all -no "{nodelist}" -q batch)" = "node[0-3]"
'

#
# Case 1: the key follows each operation
#
test_expect_success 'add writes the key' '
	flux queue add dat hosts=node[2-3] &&
	key_get >add.json &&
	test_debug "cat add.json" &&
	test "$(jq -r ".queues.dat.hosts" <add.json)" = "node[2-3]" &&
	test "$(jq -r ".queues.dat.enabled" <add.json)" = "true" &&
	test "$(jq -r ".queues.dat.started" <add.json)" = "false" &&
	test "$(jq -r ".version" <add.json)" = "1"
'

test_expect_success 'start updates the key' '
	flux queue start dat &&
	test "$(key_get | jq -r ".queues.dat.started")" = "true"
'

test_expect_success 'stop updates the key with its reason' '
	flux queue stop -m "maintenance" dat &&
	key_get >stop.json &&
	test "$(jq -r ".queues.dat.started" <stop.json)" = "false" &&
	test "$(jq -r ".queues.dat.stop_reason" <stop.json)" = "maintenance"
'

test_expect_success 'disable and enable update the key' '
	flux queue disable -m "closed" dat &&
	key_get >dis.json &&
	test "$(jq -r ".queues.dat.enabled" <dis.json)" = "false" &&
	test "$(jq -r ".queues.dat.disable_reason" <dis.json)" = "closed" &&
	flux queue enable dat &&
	test "$(key_get | jq -r ".queues.dat.enabled")" = "true"
'

test_expect_success 'remove drops the entry' '
	flux queue remove dat &&
	test "$(key_get | jq -r ".queues.dat")" = "null"
'

#
# Case 2: a failed add leaves nothing behind
#
test_expect_success 'a failed add leaves no queue, key entry, or property' '
	test_must_fail flux queue add bad hosts=nosuchhost &&
	test "$(key_get | jq -r ".queues.bad")" = "null" &&
	flux queue list -no "{queue}" >badq.out &&
	test_must_fail grep -x bad badq.out &&
	served_props >badprops.out &&
	test_must_fail grep "+bad" badprops.out
'

#
# Case 3: EBUSY.  A property update round trip takes milliseconds, so
# the window is only reachable by sending both requests before waiting
# on either.
#
test_expect_success 'a second operation in flight fails with EBUSY' '
	cat >ebusy.py <<-EOT &&
	import errno
	import flux

	h = flux.Flux()
	f1 = h.rpc("job-manager.queue-add", {"name": "r1", "hosts": "node0"})
	f2 = h.rpc("job-manager.queue-add", {"name": "r2", "hosts": "node1"})
	ok = 0
	busy = 0
	for f in (f1, f2):
	    try:
	        f.get()
	        ok += 1
	    except OSError as exc:
	        if exc.errno == errno.EBUSY:
	            busy += 1
	        else:
	            raise
	print(f"{ok} {busy}")
	EOT
	flux python ebusy.py >ebusy.out &&
	test_debug "cat ebusy.out" &&
	test "$(cat ebusy.out)" = "1 1"
'

test_expect_success 'exactly one queue was created' '
	key_get >ebusy.json &&
	test_debug "cat ebusy.json" &&
	test "$(jq -r ".queues | length" <ebusy.json)" = "1"
'

test_expect_success 'clean up the EBUSY queue' '
	for q in r1 r2; do
		flux queue remove $q 2>/dev/null || true
	done &&
	test "$(key_get | jq -r ".queues | length")" = "0"
'

#
# The standard scenario, used by the reload cases below.
#
# D1 takes one of the two dat nodes rather than both, so that a second
# dat job can still be allocated while D1 runs.  B1 takes one of the two
# donor nodes for the same reason.
#
# N.B. B4, the donor job that cannot fit while dat is started, is NOT
# submitted here.  sched-simple keeps one global FCFS queue and stops at
# the first job it cannot allocate, so a pending B4 would block every
# later job in every queue.  That much is pre-existing scheduler
# behavior, unrelated to the exclusive set: a -N1 job behind a -N4 job
# waiting on another -N4 job is blocked the same way, with no runtime
# queue in sight.
#
# sched-backfill does not rescue this either: it gates backfill on a
# shadow time for the head job, and a head job that cannot fit even
# after every running job ends never gets one.  Drained ranks produce
# the same effect, so this is pre-existing EASY-backfill behavior
# rather than something the exclusive set introduces.  See
# RUNTIME-QUEUES-BACKFILL-FINDINGS.md.
#
# B4 is therefore submitted last, by the case that checks it.
scenario_setup() {
	flux queue add dat hosts=node[2-3] &&
	D1=$(flux submit -q dat -N1 $LONG true) &&
	echo $D1 >D1 &&
	flux queue start dat &&
	flux job wait-event -t 30 $D1 start &&
	B1=$(flux submit -q batch -N1 $LONG true) &&
	echo $B1 >B1 &&
	flux job wait-event -t 30 $B1 start
}

# The ranks of the runtime queue's membership property.
dat_ranks() {
	served_props >dat_ranks.out
	jq -r '."+dat" // "none"' <dat_ranks.out
}

# Re-assert the whole scenario.  Used after each reload.
scenario_check() {
	test "$(dat_ranks)" = "2-3" &&
	test "$(sched_exclusive)" = "[\"+dat\"]" &&
	case "$(nodelist $(cat D1))" in node2|node3) ;; *) false ;; esac &&
	case "$(nodelist $(cat B1))" in node0|node1) ;; *) false ;; esac &&
	D2=$(flux submit -q dat -N1 $SHORT true) &&
	flux job status -v $D2 &&
	case "$(nodelist $D2)" in node2|node3) ;; *) false ;; esac &&
	B2=$(flux submit -q batch -N1 $SHORT true) &&
	flux job status -v $B2 &&
	case "$(nodelist $B2)" in node0|node1) ;; *) false ;; esac
}

# A donor job that needs every node must stay pending rather than be
# rejected: the nodes come back when dat stops.  This is the behavior
# the whole design exists for.  Submitted last and cancelled right
# after, since a pending job at the head of sched-simple's global FCFS
# queue blocks every job behind it.
check_donor_job_pends() {
	B4=$(flux submit -q batch -N4 $SHORT true) &&
	test_must_fail flux job wait-event -t 2 $B4 alloc &&
	test_must_fail flux job wait-event -t 0.1 $B4 exception &&
	flux cancel $B4 &&
	flux job wait-event -t 30 $B4 clean
}

scenario_teardown() {
	flux queue stop dat &&
	flux cancel $(cat D1) $(cat B1) 2>/dev/null
	flux job wait-event -t 30 $(cat D1) clean >/dev/null 2>&1
	flux job wait-event -t 30 $(cat B1) clean >/dev/null 2>&1
	flux queue remove dat
}

#
# Case 4: scheduler reload
#
test_expect_success 'set up the standard scenario' '
	scenario_setup
'

test_expect_success 'scheduler reload preserves runtime queue state' '
	flux module remove sched-simple &&
	flux module load sched-simple &&
	scenario_check
'

test_expect_success 'a donor job that needs all nodes pends after the reload' '
	check_donor_job_pends
'

#
# Case 5: resource and scheduler reload, dat started
#
test_expect_success 'resource reload preserves runtime queue state' '
	flux module remove sched-simple &&
	flux module reload resource monitor-force-up noverify &&
	flux module load sched-simple &&
	served_props >rreload.out &&
	test_debug "cat rreload.out" &&
	test "$(jq -r ".\"+dat\"" <rreload.out)" = "2-3" &&
	test "$(sched_exclusive)" = "[\"+dat\"]"
'

test_expect_success 'the resource module logged the restored queue' '
	flux dmesg | grep "queues.runtime: queue dat"
'

test_expect_success 'tear down the standard scenario' '
	scenario_teardown
'

#
# Case 6: resource and scheduler reload, dat stopped
#
test_expect_success 'a stopped queue reloads with an empty exclusive set' '
	flux queue add dat hosts=node[2-3] &&
	flux module remove sched-simple &&
	flux module reload resource monitor-force-up noverify &&
	flux module load sched-simple &&
	served_props >stopped.out &&
	test "$(jq -r ".\"+dat\"" <stopped.out)" = "2-3" &&
	test "$(sched_exclusive)" = "[]"
'

test_expect_success 'a donor job may use all nodes while dat is stopped' '
	flux queue start batch &&
	id=$(flux submit -q batch -N4 $SHORT true) &&
	flux job wait-event -t 30 $id alloc &&
	test "$(nodelist $id)" = "node[0-3]" &&
	flux job wait-event -t 30 $id clean
'

test_expect_success 'a dat job stays pending while dat is stopped' '
	id=$(flux submit -q dat -N1 $SHORT true) &&
	test_must_fail flux job wait-event -t 2 $id alloc &&
	flux cancel $id &&
	flux job wait-event -t 30 $id clean
'

test_expect_success 'remove the stopped queue' '
	flux queue remove dat
'

#
# Case 7-9: config reload
#
test_expect_success 'config reload keeps runtime queues' '
	flux queue add dat hosts=node[2-3] &&
	flux queue start dat &&
	key_get >before_conf.json &&
	flux config load <<-EOT &&
	[queues.batch]
	hosts = "all"
	[queues.spare]
	[policy.jobspec.defaults.system]
	queue = "batch"
	EOT
	flux queue list -no "{queue}" >conf_queues.out &&
	grep -x dat conf_queues.out &&
	grep -x spare conf_queues.out &&
	key_get >after_conf.json &&
	test_cmp before_conf.json after_conf.json &&
	test "$(sched_exclusive)" = "[\"+dat\"]"
'

test_expect_success 'config reload with a name conflict fails' '
	test_must_fail flux config load 2>conflict.err <<-EOT &&
	[queues.batch]
	hosts = "all"
	[queues.dat]
	hosts = "node[0-1]"
	EOT
	test_debug "cat conflict.err" &&
	grep "conflicts with runtime queue" conflict.err
'

test_expect_success 'the runtime queue is unchanged after the failed reload' '
	flux queue list -no "{queue}" | grep -x dat &&
	key_get >after_conflict.json &&
	test_cmp before_conf.json after_conflict.json
'

test_expect_success 'config reload with no named queues fails' '
	test_must_fail flux config load 2>anon.err </dev/null &&
	test_debug "cat anon.err" &&
	grep "runtime queues require named queues" anon.err &&
	flux queue list -no "{queue}" | grep -x dat
'

#
# Case 10-11: job-manager reload
#
# N.B. a job-manager reload leaves configured queues stopped too, so the
# scenario here uses pending jobs only.
#
test_expect_success 'capture state before the job-manager reload' '
	served_props >jm_before_props.out &&
	sched_exclusive >jm_before_excl.out &&
	key_get >jm_before_key.json
'

# N.B. the scheduler must be reloaded after the job-manager: it stays
# loaded but loses its connection, and every queue then reports
# "Scheduler is offline" until it reattaches.
test_expect_success 'job-manager reload recreates the runtime queue' '
	flux module reload job-manager &&
	flux module reload sched-simple &&
	flux queue list -no "{queue}" | grep -x dat &&
	flux dmesg | grep "restored runtime queue dat"
'

test_expect_success 'R and the exclusive set are untouched' '
	served_props >jm_after_props.out &&
	sched_exclusive >jm_after_excl.out &&
	test_cmp jm_before_props.out jm_after_props.out &&
	test_cmp jm_before_excl.out jm_after_excl.out
'

test_expect_success 'the key is unchanged' '
	key_get >jm_after_key.json &&
	test_cmp jm_before_key.json jm_after_key.json
'

# N.B. the queues are started by name: --all is rejected while an
# exclusive queue exists, since it would need one update per queue.
# The queue kept its started state across the reload, so a job submitted
# to it runs without any further administrative action.
#
# N.B. the ranks come from the alloc event rather than from flux-jobs:
# job-list is reloaded with the job-manager and no longer knows about
# jobs submitted before it.  The job is only followed as far as alloc,
# which is what this is testing; a reloaded job-manager does not drive
# the job to completion.
test_expect_success 'a dat job is allocated dat nodes after the reload' '
	id=$(flux submit -q dat -N1 $SHORT true) &&
	flux job wait-event -t 30 -f json $id alloc >alloc.out &&
	test_debug "cat alloc.out" &&
	grep -E "rank[23]/" alloc.out &&
	flux cancel $id 2>/dev/null || true
'

test_expect_success 'disable survives a job-manager reload' '
	flux queue disable -m "closed for the night" dat &&
	flux module reload job-manager &&
	flux module reload sched-simple &&
	flux queue list -no "{queue} {enabled}" | grep "^dat" >dis_after.out &&
	test_debug "cat dis_after.out" &&
	test_must_fail flux submit -q dat -N1 $SHORT true 2>dis_sub.err &&
	test_debug "cat dis_sub.err" &&
	grep "closed for the night" dis_sub.err &&
	flux queue enable dat
'

#
# Case 12: remove still works after all of that
#
test_expect_success 'remove works after the reloads' '
	flux queue start batch &&
	flux queue remove dat &&
	flux queue list -no "{queue}" >finalq.out &&
	test_must_fail grep -x dat finalq.out &&
	served_props >final.out &&
	test_must_fail grep "+dat" final.out &&
	test "$(sched_exclusive)" = "[]" &&
	test "$(key_get | jq -r ".queues | length")" = "0"
'

test_done
