#!/bin/sh

test_description='Test runtime queues across instance restart'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. `dirname $0`/sharness.sh

# Disable the rc1 cleanup script that cancels running jobs on shutdown,
# so that jobs remain in RUN state across a restart, as in t3202.
export FLUX_DISABLE_JOB_CLEANUP=t

# Fake resources give four distinct hostnames on a single broker, so a
# hostlist resolves to a subset of ranks.  Every start below uses the
# same options, so each one is a lifetime of the same instance.
CONF="--conf=fake-resources.nnodes=4 \
--conf=fake-resources.cores-per-node=2 \
--conf=fake-resources.host-prefix=node \
--conf=queues.batch.hosts=all \
--conf=policy.jobspec.defaults.system.queue=batch"

LONG="--flags=debug --setattr=system.exec.test.run_duration=100s"
SHORT="--setattr=system.exec.test.run_duration=0.01s"

# Usage: instance SCRIPT
#   Run one lifetime of the persistent instance.
instance() {
	flux start --setattr=statedir=$(pwd) $CONF "$@"
}

# Helpers for use inside an instance, written to a file so that each
# lifetime can source them.
test_expect_success 'write the in-instance helpers' '
	cat >helpers.sh <<-"EOT"
	served_props() {
		flux python -c \
	"import flux, json; print(json.dumps(flux.Flux().rpc(\"resource.get\",{}).get()[\"R\"][\"execution\"].get(\"properties\"), sort_keys=True))"
	}
	dat_ranks() {
		served_props | jq -r ".\"+dat\" // \"none\""
	}
	sched_exclusive() {
		flux python -c \
	"import flux, json; print(json.dumps(sorted(flux.Flux().rpc(\"sched.resource-status\").get()[\"exclusive\"])))"
	}
	key_get() {
		flux kvs get queues.runtime 2>/dev/null
	}
	EOT
	test -f helpers.sh
'

#
# Case 1: clean restart, dat stopped, pending job
#
test_expect_success 'run 1: add dat and submit a job that cannot run yet' '
	instance sh -c ". ./helpers.sh &&
	    flux queue start --all &&
	    flux queue add dat hosts=node[2-3] &&
	    flux submit -q dat -N2 hostname >D1.id &&
	    ! flux job wait-event -t 2 \$(cat D1.id) alloc" &&
	test -s D1.id
'

test_expect_success 'run 2: the queue, its property, and the job are back' '
	instance sh -c ". ./helpers.sh &&
	    flux queue list -no \"{queue}\" >q.out &&
	    grep -x dat q.out &&
	    test \"\$(dat_ranks)\" = \"2-3\" &&
	    test \"\$(sched_exclusive)\" = \"[]\" &&
	    flux jobs -no \"{state} {queue}\" \$(cat D1.id) >D1.state" &&
	test_debug "cat D1.state" &&
	grep "SCHED dat" D1.state
'

test_expect_success 'run 3: starting the queue runs the pending job' '
	instance sh -c ". ./helpers.sh &&
	    flux queue start batch &&
	    flux queue start dat &&
	    flux job wait-event -t 30 \$(cat D1.id) alloc &&
	    flux job wait-event -t 30 \$(cat D1.id) clean &&
	    flux jobs -no \"{nodelist}\" \$(cat D1.id) >D1.nodes" &&
	test_debug "cat D1.nodes" &&
	test "$(cat D1.nodes)" = "node[2-3]"
'

#
# Case 2: clean restart with dat started and a running job
#
test_expect_success 'run 4: start dat and leave a job running' '
	instance sh -c ". ./helpers.sh &&
	    flux queue start batch &&
	    flux submit -q dat -N1 $LONG true >D2.id &&
	    flux job wait-event -t 30 \$(cat D2.id) start &&
	    flux submit -q batch -N1 $LONG true >B1.id &&
	    flux job wait-event -t 30 \$(cat B1.id) start" &&
	test -s D2.id && test -s B1.id
'

test_expect_success 'run 5: the started queue and its exclusive set are back' '
	instance sh -c ". ./helpers.sh &&
	    test \"\$(dat_ranks)\" = \"2-3\" &&
	    test \"\$(sched_exclusive)\" = \"[\\\"+dat\\\"]\" &&
	    test \"\$(key_get | jq -r .queues.dat.started)\" = \"true\" &&
	    flux job eventlog \$(cat D2.id) >D2.log &&
	    flux job eventlog \$(cat B1.id) >B1.log" &&
	grep "reattach-finish" D2.log &&
	grep "reattach-finish" B1.log
'

test_expect_success 'run 6: a donor job still avoids the exclusive nodes' '
	instance sh -c ". ./helpers.sh &&
	    flux submit -q batch -N1 $SHORT true >B2.id &&
	    flux job wait-event -t 30 -f json \$(cat B2.id) alloc >B2.alloc" &&
	test_debug "cat B2.alloc" &&
	grep -E "rank[01]/" B2.alloc
'

# The donor job that needs every node must stay pending rather than be
# rejected: the nodes come back when dat stops.
test_expect_success 'run 7: a donor job that needs all nodes stays pending' '
	instance sh -c ". ./helpers.sh &&
	    flux submit -q batch -N4 $SHORT true >B4.id &&
	    ! flux job wait-event -t 2 \$(cat B4.id) alloc &&
	    ! flux job wait-event -t 0.1 \$(cat B4.id) exception"
'

# Stopping the queue releases its property from the exclusive set, so
# the nodes are available to the donor queue again.
#
# N.B. this does NOT then wait for B4, the job left pending by the
# previous lifetime.  A job that was already pending when the instance
# restarted is not reconsidered when resources free up later in the new
# lifetime, so it stays in SCHED.  That is pre-existing behavior with
# no runtime queue involved: an -N4 job pending behind another -N4 job
# across a restart behaves the same way.  A job submitted after the
# nodes are free runs, which is what is checked here.
test_expect_success 'run 8: stopping dat releases the exclusive set' '
	instance sh -c ". ./helpers.sh &&
	    flux queue stop dat &&
	    flux cancel \$(cat D2.id) \$(cat B1.id) &&
	    flux job wait-event -t 30 \$(cat D2.id) clean &&
	    flux job wait-event -t 30 \$(cat B1.id) clean &&
	    test \"\$(sched_exclusive)\" = \"[]\" &&
	    test \"\$(dat_ranks)\" = \"2-3\" &&
	    flux cancel \$(cat B4.id)"
'

test_expect_success 'run 9: a donor job may use every node again' '
	instance sh -c ". ./helpers.sh &&
	    flux submit -q batch -N4 $SHORT true >B5.id &&
	    flux job wait-event -t 30 \$(cat B5.id) alloc &&
	    flux job wait-event -t 30 \$(cat B5.id) clean &&
	    flux jobs -no \"{nodelist}\" \$(cat B5.id) >B5.nodes" &&
	test "$(cat B5.nodes)" = "node[0-3]"
'

#
# Case 3: clean restart after remove
#
test_expect_success 'run 10: remove the queue' '
	instance sh -c ". ./helpers.sh &&
	    flux queue remove dat &&
	    test \"\$(key_get | jq -r \".queues | length\")\" = \"0\""
'

test_expect_success 'run 11: the queue and its property stay gone' '
	instance sh -c ". ./helpers.sh &&
	    flux queue list -no \"{queue}\" >q2.out &&
	    ! grep -x dat q2.out &&
	    test \"\$(dat_ranks)\" = \"none\" &&
	    test \"\$(sched_exclusive)\" = \"[]\" &&
	    flux queue start batch &&
	    flux run -q batch -N4 $SHORT true"
'

#
# Case 4: a configured queue of the same name appears while the
# instance is down.  The configuration wins, in both modules.
#
test_expect_success 'run 12: add a runtime queue' '
	instance sh -c ". ./helpers.sh &&
	    flux queue add dat hosts=node[2-3] &&
	    test \"\$(key_get | jq -r .queues.dat.hosts)\" = \"node[2-3]\""
'

test_expect_success 'run 13: a configured queue of the same name wins' '
	flux start --setattr=statedir=$(pwd) $CONF \
	    --conf=queues.dat.hosts=node[0-1] \
	    sh -c ". ./helpers.sh &&
	        test \"\$(dat_ranks)\" = \"0-1\" &&
	        flux dmesg >dmesg.out" &&
	test_debug "grep -i conflict dmesg.out" &&
	grep "queues.runtime: queue dat conflicts" dmesg.out &&
	grep "runtime queue dat conflicts with configured queue" dmesg.out
'

test_expect_success 'run 14: the stale entry is dropped by the next save' '
	flux start --setattr=statedir=$(pwd) $CONF \
	    --conf=queues.dat.hosts=node[0-1] \
	    sh -c ". ./helpers.sh &&
	        flux queue add tmp hosts=node2 &&
	        flux queue remove tmp &&
	        key_get >key13.json" &&
	test_debug "cat key13.json" &&
	test "$(jq -r ".queues.dat" <key13.json)" = "null"
'

#
# Case 5: crash after a checkpoint.  The state as of the checkpoint is
# recovered, which includes the key.
#
test_expect_success 'run 15: crash the broker after a checkpoint' '
	rm -rf crash && mkdir crash && (cd crash &&
	    cp ../helpers.sh . &&
	    test_expect_code 137 flux start --setattr=statedir=$(pwd) $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue start --all &&
	            flux queue add dat hosts=node[2-3] &&
	            flux queue start dat &&
	            flux submit -q dat -N1 $LONG true >D.id &&
	            flux job wait-event -t 30 \$(cat D.id) start &&
	            flux kvs put --sync dummy=1 &&
	            kill -9 \$(flux getattr broker.pid)")
'

test_expect_success 'run 16: the queue, key, property, and set are restored' '
	(cd crash &&
	    flux start --setattr=statedir=$(pwd) $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue list -no \"{queue}\" >q.out &&
	            grep -x dat q.out &&
	            test \"\$(key_get | jq -r .queues.dat.started)\" = \"true\" &&
	            test \"\$(dat_ranks)\" = \"2-3\" &&
	            test \"\$(sched_exclusive)\" = \"[\\\"+dat\\\"]\"")
'

#
# Case 6: crash before any checkpoint.  Nothing is recovered, and the
# queue and its jobs are lost together rather than one without the
# other.
#
test_expect_success 'run 17: crash the broker with no checkpoint' '
	rm -rf nocp && mkdir nocp && (cd nocp &&
	    cp ../helpers.sh . &&
	    test_expect_code 137 flux start --setattr=statedir=$(pwd) $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue start --all &&
	            flux queue add dat hosts=node[2-3] &&
	            flux submit -q dat -N1 $LONG true >D.id &&
	            kill -9 \$(flux getattr broker.pid)")
'

test_expect_success 'run 18: the queue and its job are lost together' '
	(cd nocp &&
	    flux start --setattr=statedir=$(pwd) $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue list -no \"{queue}\" >q.out &&
	            ! grep -x dat q.out &&
	            test \"\$(dat_ranks)\" = \"none\" &&
	            ! flux jobs -no \"{state}\" \$(cat D.id)")
'

#
# Case 7: dump and restore
#
test_expect_success 'run 19: dump an instance with a started runtime queue' '
	rm -rf dumpdir && mkdir dumpdir && (cd dumpdir &&
	    cp ../helpers.sh . &&
	    flux start -Scontent.dump=dump.tar $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue start --all &&
	            flux queue add dat hosts=node[2-3] &&
	            flux queue start dat &&
	            key_get >key_before.json") &&
	test -f dumpdir/dump.tar
'

test_expect_success 'run 20: the restored instance has the same state' '
	(cd dumpdir &&
	    flux start -Scontent.restore=dump.tar $CONF \
	        sh -c ". ./helpers.sh &&
	            flux queue list -no \"{queue}\" >q.out &&
	            grep -x dat q.out &&
	            test \"\$(dat_ranks)\" = \"2-3\" &&
	            test \"\$(sched_exclusive)\" = \"[\\\"+dat\\\"]\" &&
	            key_get >key_after.json") &&
	test_cmp dumpdir/key_before.json dumpdir/key_after.json
'

test_done
