# Runtime queues: a pending donor job starves the DAT queue

A blocking finding for the prototype, and for the design discussion.
**This is not caused by the exclusive set**, but it does make the
feature unusable with `sched-simple` as it stands, because it defeats
the admin's purpose: the nodes are set aside, they are idle, and the
users they were set aside for still cannot run.

## The failure

Four nodes. `[queues.batch] hosts = "all"`. A runtime queue `tmp` on
ranks 2-3, started, so ranks 2-3 are reserved for `tmp`'s jobs.

```
1. submit -q batch -N4   ->  SCHED   (correct: cannot fit while tmp holds 2-3)
2. submit -q tmp   -N2   ->  SCHED   (WRONG: ranks 2-3 are idle and reserved
                                      for exactly this job)
   idle nodes while the DAT job waits: 4
```

Cancel the batch job and the DAT job starts immediately. So the DAT
job is not infeasible or mis-filtered -- it is simply never considered
while a job it has nothing to do with sits at the head of the queue.

This is the whole point of the feature, inverted: an admin sets nodes
aside for a user group, and an unrelated pending job in the donor queue
prevents that group from using them.

## It is not the exclusive set

The identical failure occurs with two ordinary configured queues and no
exclusive set anywhere:

```sh
flux start -s4 --conf=queues.batch.hosts=all --conf=queues.other.hosts=2-3 \
               --conf=policy.jobspec.defaults.system.queue=batch
```

```
submit -q batch -N4  ->  SCHED
submit -q other -N2  ->  SCHED     # same starvation, no E involved
```

So this is a pre-existing `sched-simple` property, not something the
runtime-queue work introduces. It is also not specific to runtime
queues: any site running two disjoint configured queues under
`sched-simple` has it today.

## Cause

`sched-simple` (via `sched-fifo.py`) keeps **one global FIFO** across
every Flux queue and stops the pass at the first job it cannot
allocate:

```python
while self._queue:
    job = self._queue[0]
    ...                     # stops here if the head cannot be allocated
```

There is no per-queue scheduling: the base `Scheduler._queue` is a
single heap, and jobs carry no queue identity into it. A job that
cannot run therefore blocks every job behind it regardless of which
Flux queue each belongs to.

Neither documented escape helps:

| scheduler / mode | DAT job runs? |
| --- | --- |
| `sched-simple` (default, queue-depth=8) | no |
| `sched-simple mode=unlimited` | no |
| `sched-backfill` with durations | no (see the backfill findings) |

`sched-backfill` fails for a related reason: it gates all backfill on a
shadow time for the head job, and a head job that cannot fit once every
running job has ended never gets one. Drained nodes have the same
effect there, which is the subject of
`RUNTIME-QUEUES-BACKFILL-FINDINGS.md`.

## What this means for the design

The exclusive-set mechanism itself works. Membership, the access rule,
persistence, and restart are all demonstrated. But a prototype that an
admin cannot actually use is not a successful prototype, and with
`sched-simple` this one is not usable: the demonstration sequence only
succeeds if the DAT job happens to be submitted *before* any donor job
that cannot fit, which no admin can arrange.

Three ways out, in rough order of effort:

1. **Per-queue scheduling in `sched-simple`.** Schedule each Flux queue
   independently -- or at least do not let a job in queue A block a job
   in queue B. This is the real fix for the prototype, and it would
   benefit every multi-queue `sched-simple` user, not just this
   feature. The alloc request would need to carry the job's queue,
   which it now does for the exclusive filter (`ResourceRequest.queue`),
   so the information is already present.

2. **Confirm Fluxion does not have this.** Fluxion's qmanager does
   schedule per queue, so this failure may be specific to
   `sched-simple`. If so, the design is sound and the prototype's
   limitation is a statement about its scheduler, not about E. **This
   should be checked before any design conclusion is drawn** -- it is
   the single most important open question.

3. **Document it as an MVP limit.** Only honest if Fluxion is
   unaffected, since a system instance runs Fluxion. The current MVP
   limit text is too weak either way: it says a large donor job "blocks
   its queue", when in fact it blocks *every* queue.

## Reproducer

```sh
flux start -s4 --conf=queues.batch.hosts=all \
    --conf=policy.jobspec.defaults.system.queue=batch sh -c '
S="--setattr=system.exec.test.run_duration=0.01s"
flux queue start --all
flux queue add tmp targets=2-3
flux queue start tmp
B=$(flux submit -q batch -N4 $S true); sleep 2
echo "batch -N4: $(flux jobs -no "{state}" $B)"
T=$(flux submit -q tmp -N2 $S true)
flux job wait-event -t 8 $T alloc; echo "dat job rc=$?"
echo "idle nodes: $(flux resource list -s free -no "{nnodes}")"'
```

Swap the runtime queue for `--conf=queues.other.hosts=2-3` and submit
`-q other` to see the same result with no exclusive set.
