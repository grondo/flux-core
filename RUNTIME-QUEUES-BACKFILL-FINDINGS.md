# Runtime queues: backfill interaction with the exclusive set

Findings from the flux-core prototype on branch `runtime-queue-proto`.
Written up for the design discussion. The short version: **the earlier
"backfill is broken by E" alarm was wrong.** E behaves exactly like
drain. The real limitation is pre-existing in `sched-backfill` and is
not specific to this design.

## Summary

| claim | verdict |
|---|---|
| Runtime queues underperform normal queues in ordinary use | **No.** Both queues' jobs flow at full rate. |
| E disables backfill where drain does not | **No.** Drain and E behave identically. |
| A head job that cannot fit disables backfill behind it | **Yes**, and this is pre-existing, independent of E. |

## What was actually measured

All on a real 4-broker instance (`flux start -s4`) with R seeded as
`node[0-3]`, 2 cores each, `sched-backfill` loaded, and all jobs
carrying `-t` durations so backfill is eligible.

Configuration: `[queues.batch] hosts = "all"`, plus a runtime queue
where noted.

### 1. Normal operation is unaffected

With `dat` started and exclusive on `node[2-3]`:

- three consecutive `dat` jobs (`-N1`): all ran on `node[2-3]`;
- three consecutive `batch` jobs (`-N2`): all ran on `node[0-1]`.

No delays, no pending. This is the DAT use case, and it works at full
rate. Nothing about E slows down the traffic it is designed to carry.

### 2. Drain and E behave identically

One running `-N1` job, then a `-N4` head job that cannot fit, then a
small `-N1` job submitted behind it:

| condition | head job gets `t_estimate` | small job backfills |
|---|---|---|
| no drain, no E (control) | yes | **yes**, onto node2 |
| `flux resource drain 2-3` | no | no |
| `dat` started on `node[2-3]` | no | no |

Drain and E give the same result. The control shows backfill working
normally on the same instance, so the harness is sound.

### 3. The actual rule

Backfill is disabled behind a head job exactly when that head job
cannot fit **even after every running job ends**. Verified directly:
with `dat` exclusive on `node3` only, a `-N3` head job (which fits in
the three remaining non-exclusive nodes once the running job ends) gets
a shadow time, and backfill works:

```
head(-N3, fits after B1 ends) annot: {"sched": {"t_estimate": ...}}
backfill rc=0 nodes=node1
```

So the condition is about *permanent* unfittability, not about E.
`_shadow_time()` advances a simulated pool through job expirations; if
the head job still does not fit once all of them are exhausted, the
shadow is `None`, and `schedule()` gates all backfill on
`shadow is not None`. Drained nodes and exclusive nodes both produce a
permanently-unfittable head job, so both hit the same path.

This is the documented EASY-backfill behavior, not a defect introduced
here. A cluster with any permanently unavailable capacity has it today.

## Two false leads, recorded so they are not re-chased

### Fake resources do not propagate drain to the scheduler

With `--conf=fake-resources.*`, `flux resource drain 2-3` marks the
nodes drained in the resource module, but the scheduler's `down` set
stays empty and a `-N4` job **runs on the drained nodes**:

```
flux resource list -s down: [node[2-3]]
-N4 with NOTHING else running: rc=0 nodes=node[0-3]
```

On a real instance the same sequence gives `down: ['2-3']` and the job
stays pending. This is why the first drain-vs-E comparison appeared to
show a difference: the drain arm was not actually constraining the
scheduler. Any test comparing drain against E must run on a real
instance, or verify through `sched.resource-status` that the scheduler
sees the drain.

Worth confirming whether this is intended (`monitor-force-up` implies
the nodes are up) or a gap in the fake-resources path.

### "E has no end time" is not the mechanism

An earlier note claimed the shadow-time simulation fails because E has
no expiration to advance to. That is not the distinguishing factor:
driving `_shadow_time()`'s loop directly against a pool produces
`shadow = None` identically for a drained pool and an exclusive one.
Both fail for the same reason — the head job is permanently unfittable
— not because E is special.

## Implications for the design

1. **E does not need a time bound to make backfill work.** The
   "time-bounded E" item under Future work is still worth having for
   planning quality, but it is not a prerequisite, and backfill is not
   the argument for it.

2. **Rule 1's "behaves like a drained rank" holds** on this axis. That
   is the useful framing: whatever a site accepts today for drained
   capacity, it gets for exclusive capacity.

3. **The MVP limit as written is accurate** and needs no change:

   > With an FCFS queue policy, a large donor job that cannot fit while
   > an exclusive queue is started blocks its queue until that queue
   > stops. Drained nodes cause the same behavior today.

   The only correction is that backfill does not lift this limit —
   because it does not lift it for drain either.

4. **Nothing here blocks proceeding with E.** If the permanently-
   unfittable-head-job behavior is worth improving, that is a
   `sched-backfill` change (let the reservation form past indefinitely
   unavailable capacity, or skip the head job and reserve for the next
   fittable one) that benefits drain and E equally, and is independent
   of this design.

## Open question for Fluxion

The design issue says Fluxion's backfill policies "already move jobs
that cannot reserve to the blocked list" and "already reconsider those
jobs on resource notify". If so, Fluxion may not have this limitation
at all. Worth confirming, since it determines whether this is a
Python-scheduler-only characteristic.

## Reproducer

```sh
flux start -s4 --conf=queues.batch.hosts=all \
    --conf=policy.jobspec.defaults.system.queue=batch sh -c '
flux module remove sched-simple
flux R encode -r 0-3 -H node[0-3] -c 0-1 | flux kvs put -r resource.R=-
flux module reload resource monitor-force-up noverify
flux module load sched-backfill
flux queue start --all
flux queue add dat hosts=node[2-3]; flux queue start dat
L="--flags=debug --setattr=system.exec.test.run_duration=100s -t 5m"
S="--setattr=system.exec.test.run_duration=0.01s -t 10s"
B1=$(flux submit -q batch -N1 $L true); flux job wait-event -t 20 $B1 start
B4=$(flux submit -q batch -N4 $S true); sleep 2
echo "head annot: $(flux jobs -no "{annotations}" $B4)"
B2=$(flux submit -q batch -N1 $S true)
flux job wait-event -t 8 $B2 alloc; echo "rc=$?"'
```

Swap `flux queue add dat ...` for `flux resource drain 2-3 maint` to
get the drain arm, and drop both for the control.
