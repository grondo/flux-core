#!/usr/bin/env python3
###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################
"""Generate synthetic job-list.list responses for flux-jobs --from-stdin.

Used to benchmark flux-jobs output generation without requiring a
running Flux instance with a large number of jobs.
"""

import argparse
import json
import os
import random
import sys

QUEUES = ("batch", "debug", "pdebug", "pbatch")
NAMES = ("hostname", "sleep", "mpi_hello", "my-really-long-job-name", "sh")

parser = argparse.ArgumentParser()
parser.add_argument("-n", "--count", type=int, default=40000)
parser.add_argument("-s", "--seed", type=int, default=42)
parser.add_argument(
    "--inactive-fraction",
    type=float,
    default=0.9,
    help="fraction of jobs that are INACTIVE (rest are split pending/running)",
)
args = parser.parse_args()

random.seed(args.seed)
userid = os.getuid()
t0 = 1700000000.0

for i in range(args.count):
    t_submit = t0 + i
    job = {
        "id": 10000000000000 + i * 1000,
        "userid": userid,
        "urgency": 16,
        "priority": random.randint(0, 4294967295),
        "t_submit": t_submit,
        "t_depend": t_submit,
        "t_priority": t_submit,
        "name": random.choice(NAMES),
        "queue": random.choice(QUEUES),
        "cwd": "/home/user/work",
        "ntasks": random.choice((1, 2, 4, 8, 128)),
        "ncores": random.choice((1, 2, 4, 8, 128)),
        "duration": random.choice((0.0, 60.0, 3600.0)),
    }
    r = random.random()
    if r < args.inactive_fraction:
        # INACTIVE
        job["state"] = 64
        job["t_sched"] = t_submit + 1
        job["t_run"] = t_submit + 2
        job["t_cleanup"] = t_submit + 2 + random.randint(1, 3600)
        job["t_inactive"] = job["t_cleanup"] + 0.1
        job["nnodes"] = random.choice((1, 2, 4))
        job["ranks"] = "0-3"
        job["nodelist"] = "fluke[0-3]"
        job["expiration"] = job["t_run"] + 3600
        if random.random() < 0.8:
            job["result"] = 1  # COMPLETED
            job["success"] = True
            job["waitstatus"] = 0
        else:
            job["result"] = 2  # FAILED
            job["success"] = False
            job["waitstatus"] = 256
            job["exception_occurred"] = True
            job["exception_severity"] = 0
            job["exception_type"] = "exec"
            job["exception_note"] = "task 0 exited with rc=1"
        job["annotations"] = {"user": {}, "sched": {}}
    elif r < args.inactive_fraction + (1 - args.inactive_fraction) / 2:
        # RUNNING
        job["state"] = 32
        job["t_sched"] = t_submit + 1
        job["t_run"] = t_submit + 2
        job["nnodes"] = random.choice((1, 2, 4))
        job["ranks"] = "0-3"
        job["nodelist"] = "fluke[0-3]"
        job["expiration"] = t0 + 1e9
        job["annotations"] = {"sched": {"resource_summary": "rank[0-3]/core[0-1]"}}
    else:
        # SCHED
        job["state"] = 8
        job["t_sched"] = t_submit + 1
        job["annotations"] = {
            "sched": {
                "reason_pending": "insufficient resources",
                "t_estimate": t0 + 1e9,
            }
        }
    sys.stdout.write(json.dumps(job))
    sys.stdout.write("\n")
