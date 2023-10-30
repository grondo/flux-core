#!/bin/sh
test_description='Test update of running jobs'

. $(dirname $0)/sharness.sh

if flux job submit --help 2>&1 | grep -q sign-type; then
	test_set_prereq HAVE_FLUX_SECURITY
fi

test_under_flux 1 job

flux setattr log-stderr-level 1
export FLUX_URI_RESOLVE_LOCAL=t
runas_guest() {
	local userid=$(($(id -u)+1))
        FLUX_HANDLE_USERID=$userid FLUX_HANDLE_ROLEMASK=0x2 "$@"
}

submit_job_as_guest()
{
	local duration=$1
	local userid=$(($(id -u)+1))
        flux run --dry-run -t $duration \
	  --setattr=exec.test.run_duration=\"600\" \
	  sleep inf | \
          flux python ${SHARNESS_TEST_SRCDIR}/scripts/sign-as.py $userid \
            >job.signed
        FLUX_HANDLE_USERID=$userid \
          flux job submit --flags=signed job.signed
}

# Usage: job_manager_get_R ID
job_manager_get_R() {
	flux python -c "import flux; print(flux.Flux().rpc(\"job-manager.getattr\",{\"id\":$(flux job id $1),\"attrs\":[\"R\"]}).get_str())"
}
job_manager_get_expiration() {
	job_manager_get_R $1 | jq .R.execution.expiration
}
job_manager_get_starttime() {
	job_manager_get_R $1 | jq .R.execution.starttime
}

test_expect_success 'instance owner can adjust expiration of their own job' '
	jobid=$(flux submit --wait-event=start -t5m sleep 300) &&
	expiration=$(job_manager_get_expiration $jobid) &&
	test_debug "echo expiration=$expiration" &&
	flux update $jobid duration=+1m &&
	job_manager_get_expiration $jobid | jq -e ". == $expiration + 60"
'
test_expect_success 'duration update with expiration in past fails' '
	test_must_fail flux update $jobid duration=10ms 2>err.log &&
	grep past err.log
'
test_expect_success 'duration update is processed by execution system' '
	duration=$(job_manager_get_starttime $jobid | jq "now - . + 3") &&
	flux update $jobid duration=$duration &&
	flux job wait-event -vt5 -m type=timeout $jobid exception
'
#  Test that the job shell correctly processes an expiration update.
#  Set up the job shell to send SIGTERM to the job 60s before expiration.
#  for a job with a 5m duration. Then adjust duration such that expiration
#  will occur 30s from now, and ensure the job shell picks up the update and
#  sends SIGTERM immediately.
#
test_expect_success 'duration update is processed by job shell' '
	jobid=$(flux submit --wait-event start \
		-o verbose --signal=SIGTERM@60 -t5 sleep 300) &&
	duration=$(job_manager_get_starttime $jobid | jq "now - . + 30") &&
	flux update $jobid duration=$duration &&
	test_must_fail_or_be_terminated \
		flux job attach $jobid >shell.log 2>&1 &&
	test_debug "cat shell.log" &&
	grep "Will send SIGTERM to job in 0\.0" shell.log &&
	grep "sending SIGTERM" shell.log
'
test_expect_success HAVE_FLUX_SECURITY 'duration update of running job is denied for guest' '
	jobid=$(submit_job_as_guest 5m) &&
	flux job wait-event -vt 30 $jobid start &&
	test_must_fail runas_guest flux update $jobid duration=+1m &&
	flux cancel $jobid
'
test_expect_success HAVE_FLUX_SECURITY 'duration update of running guest job is allowed for owner' '
	jobid=$(submit_job_as_guest 5m) &&
	flux job wait-event -vt 30 $jobid start &&
	expiration=$(job_manager_get_expiration $jobid) &&
	flux update $jobid duration=+1m &&
	job_manager_get_expiration $jobid | jq -e ". == $expiration + 60" &&
	flux cancel $jobid
'
#  Set module debug flag 0x4 on sched-simple so it will deny sched.expiration
#  RPCs:
test_expect_success 'expiration update can be denied by scheduler' '
	flux module debug --set 4 sched-simple &&
	jobid=$(flux submit --wait-event=start -t 5m sleep 300) &&
	test_must_fail flux update $jobid duration=+10m >sched-deny.out 2>&1 &&
	test_debug "cat sched-deny.out" &&
	grep "scheduler refused" sched-deny.out &&
	flux cancel $jobid &&
	flux module debug --clear sched-simple
'
subinstance_get_R() {
	flux proxy $1 flux kvs get resource.R
}
subinstance_get_expiration() {
	subinstance_get_R $1 | jq .execution.expiration
}
#
# Test duration/expiration propagation:
# 1. submit an instance wtih 5m time limit
# 2. submit a job in that instance with no duration - its expiration should
#    be set to that of the instance. (<5m)
# 3. update duration of instance to 10m (+5m)
# 4. wait for instance resource module to post resource-update event
#    which indicates the expiration update is reflected in resource.R.
# 5. wait for sched-simple to register expiration update by checking
#    for expected log message
# 6. Ensure subinstance now reflects updated expiration in resource.R.
# 7. Ensure new expiration is 300s greater than old expiration
#
test_expect_success 'expiration update is detected by subinstance' '
	id=$(flux alloc --bg -t5m -n2) &&
	exp1=$(subinstance_get_expiration $id) &&
	test_debug "echo instance expiration is $exp1" &&
	id1=$(flux proxy $id flux submit sleep 300) &&
	timeleft1=$(flux proxy $id flux job timeleft $id1) &&
	test_debug "flux proxy $id flux job info $id1 R | jq .execution.expiration" &&
	test_debug "echo timeleft of job1 is $timeleft1" &&
	echo $timeleft | jq -e ". < 300" &&
	flux update $id duration=+5m &&
	flux proxy $id flux kvs eventlog wait-event -vt 30 \
		resource.eventlog resource-update &&
	flux proxy $id $SHARNESS_TEST_SRCDIR/scripts/dmesg-grep.py \
		-vt 30 \"sched-simple.*expiration updated\" &&
	exp2=$(subinstance_get_expiration $id) &&
	subinstance_get_R $id &&
	test_debug "echo expiration updated from $exp1 to $exp2" &&
	echo $exp2 | jq -e ". == $exp1 + 300"
'
#
# 8. Submit job to previous instance and ensure its expiration matches
#    the updated value
# 9. Ensure flux-job timeleft returns > 5m for the new job
# 10. Wait for expected resource-update event to be propagated to the first job
# 11. Ensure the timeleft of the first job is now > 5m
#
test_expect_success 'instance expiration update propagates to jobs' '
	id2=$(flux proxy $id flux submit --wait-event=start sleep 300) &&
	timeleft=$(flux proxy $id flux job timeleft $id2) &&
	test_debug "echo timeleft of job submitted after update is $timeleft" &&
	echo $timeleft | jq -e ". > 300" &&
	flux proxy $id flux job wait-event -vt 20 \
		$id1 resource-update &&
	timeleft1=$(flux proxy $id flux job timeleft $id1) &&
	test_debug "echo timeleft of job submitted extended to $timeleft1" &&
	echo $timeleft1 | jq -e ". > 300" &&
	flux shutdown --quiet $id &&
	flux job wait-event $id clean
'
test_done
