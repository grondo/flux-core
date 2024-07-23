#!/bin/sh

test_description='Test flux job exec job cleanup via SIGKILL'

. $(dirname $0)/sharness.sh

test_under_flux 4 job

test_expect_success 'job-exec: active_ranks stat works' '
	flux submit -N4 sh -c "test \$FLUX_TASK_RANK -eq 2 && exit; sleep 30" &&
	jobid=$(flux job last) &&
	flux job wait-event -p exec -Hvt 20 $jobid shell.task-exit &&
	flux module stats job-exec \
		| jq ".jobs.${jobid}.active_ranks == \"0-1,3\"" &&
	flux cancel --all &&
	flux queue idle
'
test_expect_success 'job-exec: reload module with short kill-timeout' '
	flux module reload job-exec kill-timeout=0.1s &&
	flux module stats job-exec
'
test_expect_success 'job-exec: run test program that blocks SIGTERM' '
	id=$(flux submit --wait-event=start  -n 1 -o trap.out \
	    sh -c "trap \"echo got SIGTERM\" 15; \
	           flux kvs put pid=\$\$; \
	           sleep inf; sleep inf") &&
	ns=$(flux job namespace $id) &&
	pid=$(flux kvs get -WN ${ns} ${dir}.pid) &&
	test_debug "echo script running as pid=$pid"
'
test_expect_success 'job-exec: ensure cancellation kills job' '
	test_debug "echo Canceling $id" &&
	flux cancel $id &&
	test_debug "flux job attach -vEX $id || :" &&
	test_expect_code 137 flux job status $id &&
	test_must_fail ps -q $pid
'
test_expect_success 'job-exec: reload module with kill/term-signal=SIGURG' '
	flux module reload job-exec \
		kill-timeout=0.1s kill-signal=SIGURG term-signal=SIGURG \
		max-kill-count=3
'
test_expect_success 'job-exec: submit a job' '
	jobid=$(flux submit --wait-event=start -n1 sleep inf)
'
test_expect_success 'job-exec: job is listed in flux-module stats' '
	flux module stats job-exec | jq .jobs.$jobid
'
test_expect_success 'job-exec: get sleep PID for later cleanup' '
	sleep_pid=$(flux job hostpids $jobid | sed s/.*://)
'
test_expect_success 'job-exec: cancel test job to start kill timer' '
	flux cancel $jobid
'
check_kill_count() {
	id=$1
	stat=$2
	value=$3
	timeout=${4:-10}
	count=0
	while ! flux module stats job-exec \
		| jq -e ".jobs.${id}.${stat} >= ${value}"; do
		count=$((count+1))
		if test $count -gt $((timeout*2)); then
			echo "${stat} >= ${value} timed out after ${timeout}s"
			return 1
		fi
		sleep 0.2
		flux module stats job-exec | jq .jobs.${id}.${stat}
	done
}
test_expect_success 'job-exec: ensure kill_count > 1' '
	check_kill_count $jobid kill_count 1
'
test_expect_success 'job-exec: ensure kill_shell_count > 1' '
	check_kill_count $jobid kill_shell_count 1
'
test_expect_success 'job-exec: kill-timeout > original value (0.1)' '
	flux module stats job-exec | jq .jobs.${jobid}.kill_timeout &&
	flux module stats job-exec | jq -e ".jobs.${jobid}.kill_timeout > 0.1"
'
test_expect_success 'job-exec: wait for job to be terminated by max-kill-count' '
	flux job wait-event -vt 15 $jobid clean &&
	flux dmesg -H | grep "exceeded max kill count" &&
	flux resource drain -no {reason} | grep "unkillable processes"
'
test_expect_success 'job-exec: kill orphan sleep PID' '
	kill $sleep_pid
'
test_done
