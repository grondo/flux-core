#!/bin/sh
test_description='Test job manager housekeeping'

. $(dirname $0)/sharness.sh

test_under_flux 4

flux setattr log-stderr-level 1

# Usage: list_jobs
list_jobs () {
	flux housekeeping list -no {id}
}

# Usage: kill_all signum
kill_all () {
	flux housekeeping kill --all --signal=$1
}

# Usage: kill_job jobid signum
kill_job () {
	flux housekeeping kill --jobid=$1 --signal=$2
}

# Usage: kill_ranks idset signum
kill_ranks () {
	flux housekeeping kill --targets=$1 --signal=$2
}

# Note: the hand off of resources to housekeeping occurs just before the job
# becomes inactive, therefore it is safe to assume that housekeeping has run
# for the job if it is enclosed between successful 'wait_for_running 0' calls.
# This pattern is used repeatedly in the tests below.

# Usage: wait_for_running count
wait_for_running () {
	count=0
	while test $(list_jobs | wc -l) -ne $1; do
		count=$(($count+1));
		test $count -eq 300 && return 1 # max 300 * 0.1s sleep = 30s
		sleep 0.1
	done
}

test_expect_success 'flux-housekeeping utility exists' '
	flux housekeeping list --help &&
	flux housekeeping kill --help
'
test_expect_success 'flux-housekeeping kill fails without proper args' '
	test_must_fail flux housekeeping kill &&
	test_must_fail flux housekeeping kill --all --jobid=f1
'
test_expect_success 'dump housekeeping stats, presumed empty' '
	flux module stats job-manager | jq .housekeeping
'

# Note: the broker runs housekeeping in its cwd rather than the test script's
# (the trash dir) so $(pwd) is expanded at script creation time, but
# \$(flux getattr rank) is expanded at runtime.

test_expect_success 'create housekeeping script' '
	cat >housekeeping.sh <<-EOT &&
	#!/bin/sh
	touch $(pwd)/hkflag.\$(flux getattr rank)
	EOT
	chmod +x housekeeping.sh &&
	test_debug "cat housekeeping.sh"
'
test_expect_success 'configure housekeeping without partial release' '
	flux config load <<-EOT &&
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping.sh" ]
	EOT
	test_debug "flux config get job-manager.housekeeping"
'
test_expect_success 'run a job on broker ranks 1-2 and wait for housekeeping' '
	test_debug "flux dmesg -C" &&
	rm -f hkflag.* &&
	wait_for_running 0 &&
	flux run -N2 --requires=ranks:1-2 true &&
	wait_for_running 0
'
test_expect_success 'housekeeping script ran on ranks 1-2' '
	test_debug "flux dmesg -H" &&
	test_debug "echo $(pwd)/hkflag.*" &&
	test -f hkflag.1 -a -f hkflag.2
'
test_expect_success 'configure housekeeping with immediate release' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping.sh" ]
	release-after = "0"
	EOT
'
test_expect_success 'run a job on all four ranks and wait for housekeeping' '
	rm -f hkflag.* &&
	flux dmesg -C &&
	wait_for_running 0 &&
	flux run -n4 -N4 true &&
	wait_for_running 0
'
test_expect_success 'housekeeping script ran on ranks 0-3' '
	test_debug "flux dmesg -H" &&
	test_debug  "echo $(pwd)/hkflag.*" &&
	test -f hkflag.0 -a -f hkflag.1 -a -f hkflag.2 -a -f hkflag.3
'
test_expect_success 'nodes were returned to scheduler separately' '
	flux dmesg -H | grep sched-simple >sched.log &&
	grep "free: rank0" sched.log &&
	grep "free: rank1" sched.log &&
	grep "free: rank2" sched.log &&
	grep "free: rank3" sched.log
'
test_expect_success 'create housekeeping script with one 10s straggler' '
	cat >housekeeping2.sh <<-EOT &&
	#!/bin/sh
	rank=\$(flux getattr rank)
	test \$rank -eq 3 && sleep 10
	touch $(pwd)/hkflag.\$rank
	EOT
	chmod +x housekeeping2.sh
'
test_expect_success 'configure housekeeping with release after 5s' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping2.sh" ]
	release-after = "5s"
	EOT
'
test_expect_success 'run a job on all four ranks and wait for housekeeping' '
	rm -f hkflag.* &&
	flux dmesg -C &&
	wait_for_running 0 &&
	flux run -n4 -N4 true &&
	sleep 1 &&
	flux housekeeping list &&
	wait_for_running 0
'
test_expect_success 'dump stats' '
	flux module stats job-manager | jq .housekeeping
'
test_expect_success 'housekeeping script ran on ranks 0-3' '
	test_debug "flux dmesg -H" &&
	test_debug "echo $(pwd)/hkflag.*" &&
	test -f hkflag.0 -a -f hkflag.1 -a -f hkflag.2 -a -f hkflag.3
'
test_expect_success 'there was one alloc and two frees to the scheduler' '
	flux dmesg -H | grep sched-simple >sched2.log &&
	grep "free: rank\[0-2\]" sched2.log &&
	grep "free: rank3" sched2.log
'
test_expect_success 'configuring housekeeping with bad key fails' '
	test_must_fail flux config load 2>load.err <<-EOT &&
	[job-manager.housekeeping]
	xyz = 42
	EOT
	grep "left unpacked" load.err
'
test_expect_success 'configuring housekeeping with bad fsd fails' '
	test_must_fail flux config load 2>load2.err <<-EOT &&
	[job-manager.housekeeping]
	command = [ "/bin/true" ]
	release-after = "foo"
	EOT
	grep "FSD parse error" load2.err
'
test_expect_success 'configure housekeeping with wrong path' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "/noexist" ]
	EOT
'
test_expect_success 'run a job and ensure error was logged' '
	flux dmesg -C &&
	wait_for_running 0 &&
	flux run true &&
	wait_for_running 0 &&
	flux dmesg | grep "error launching process"
'
test_expect_success 'create housekeeping script with one failing rank (3)' '
	cat >housekeeping3.sh <<-EOT &&
	#!/bin/sh
	test \$(flux getattr rank) -ne 3
	EOT
	chmod +x housekeeping3.sh
'
test_expect_success 'configure housekeeping with one failing rank' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping3.sh" ]
	EOT
'
test_expect_success 'run a job across all ranks and wait for housekeeping' '
	flux dmesg -C &&
	wait_for_running 0 &&
	flux run -N4 true &&
	wait_for_running 0 &&
	flux dmesg | grep "nonzero exit code"
'
test_expect_success 'create housekeeping script that creates output' '
	cat >housekeeping4.sh <<-EOT &&
	#!/bin/sh
	echo housekeeping-output
	echo housekeeping-output >&2
	EOT
	chmod +x housekeeping4.sh
'
test_expect_success 'configure housekeeping to print to stdout' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping4.sh" ]
	EOT
'
test_expect_success 'run a job and ensure script output was logged' '
	flux dmesg -C &&
	wait_for_running 0 &&
	flux run true &&
	wait_for_running 0 &&
	flux dmesg | grep housekeeping-output >output &&
	test $(wc -l <output) -eq 2
'
test_expect_success 'create housekeeping script that dumps environment' '
	cat >housekeeping5.sh <<-EOT &&
	#!/bin/sh
	printenv >$(pwd)/env.out
	EOT
	chmod +x housekeeping5.sh
'
test_expect_success 'configure housekeeping to dump environment' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping5.sh" ]
	EOT
'
test_expect_success 'run a job on rank 3, wait for hk, and check environment' '
	wait_for_running 0 &&
	flux run --requires=rank:3 true &&
	wait_for_running 0 &&
	grep "^FLUX_JOB_ID=$(flux job last | flux job id --to=dec)$" env.out &&
	grep "^FLUX_JOB_USERID=$(id -u)$" env.out &&
	grep "^FLUX_URI=$(flux exec -r 3 flux getattr local-uri)$" env.out
'
test_expect_success 'configure housekeeping to sleep forever' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "sleep", "inf" ]
	EOT
'
test_expect_success 'run two jobs that trigger housekeeping' '
	wait_for_running 0 &&
	flux submit --cc=0-1 -N2 --wait true
'
test_expect_success 'housekeeping is running for 2 jobs' '
	wait_for_running 2
'
test_expect_success 'send SIGTERM to all jobs' '
	kill_all 15
'
test_expect_success 'wait for housekeeping to finish' '
	wait_for_running 0
'
test_expect_success 'run a job that trigger housekeeping' '
	wait_for_running 0 &&
	flux run -N4 true
'
test_expect_success 'housekeeping is running for 1 job' '
	wait_for_running 1
'
test_expect_success 'send SIGTERM to the job by id' '
	kill_job $(flux job last | flux job id --to=dec) 15
'
test_expect_success 'wait for housekeeping to finish' '
	wait_for_running 0
'
test_expect_success 'run 4 jobs that trigger housekeeping' '
	wait_for_running 0 &&
	flux submit --cc=0-3 -N1 true
'
test_expect_success 'housekeeping is running for 4 jobs' '
	wait_for_running 4
'
test_expect_success 'flux resource list shows 4 nodes allocated' '
	test $(flux resource list -s allocated -no {nnodes}) -eq 4
'
test_expect_success 'flux housekeeping list shows 4 jobs' '
	test_debug "flux housekeeping list" &&
	test $(flux housekeeping list -n | wc -l) -eq 4
'
test_expect_success 'send SIGTERM to the nodes by rank' '
	kill_ranks 0-3 15
'
test_expect_success 'wait for housekeeping to finish' '
	wait_for_running 0
'
test_expect_success 'flux resource list shows 0 nodes allocated' '
	test $(flux resource list -s allocated -no {nnodes}) -eq 0
'
# The following tests exercise recovery from RFC 27 hello protocol
# with partial release. Once partial release is added to RFC 27, these
# tests should be removed or changed.
test_expect_success 'configure housekeeping with immediate release' '
	flux config load <<-EOT
	[job-manager.housekeeping]
	command = [ "$(pwd)/housekeeping2.sh" ]
	release-after = "0"
	EOT
'
test_expect_success 'run job that uses 4 nodes to trigger housekeeping' '
	flux run -N4 true
'
test_expect_success 'housekeeping is running for 1 job' '
	wait_for_running 1
'
test_expect_success 'reload scheduler' '
	flux dmesg -C &&
	flux module reload -f sched-simple &&
	flux dmesg -H
'
test_expect_success 'wait for housekeeping to finish' '
	wait_for_running 0
'
test_expect_success 'housekeeping jobs were terminated due to sched reload' '
	flux dmesg | grep "housekeeping:.*will be terminated"
'
test_done
