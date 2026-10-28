## design: runtime queues MVP: exclusive use with exclusive properties

### Problem

Admins need to set aside nodes for a group of users for a fixed time, for example a dedicated access time (DAT). Today this needs a config edit and a reload of the resource and scheduler modules. Users cannot submit jobs before the change. Pending jobs in the donor queue can become infeasible and fail.

Example: on Monday, an admin plans dedicated access to `node[0-3]` for a group of users, starting Tuesday at 8am.

This issue describes the MVP design for runtime queues (#7737) for this case. The same mechanism also gives configured queues an optional exclusive mode.

### Previous mechanism, and why we dropped it

In #7737, a started runtime queue takes its nodes by **removing the donor queue's property** from them. For example, starting `dat` on `node[0-3]` removes `batch` from those nodes. Stopping `dat` adds `batch` back. The resource-module half of this is already built: `resource.property-update` and `sched.property-update`.

While working out the job-manager half, we found these problems:

1. **It changes feasibility, not only availability.** After `batch` is removed from 4 of 16 nodes, a pending 16-node `batch` job can never run. The scheduler rejects it (Fluxion rejects it permanently). We want such jobs to wait until the nodes come back.
2. **It changes non-`+` properties at runtime.** Donor properties are often plain properties such as `batch`, set in `[[resource.config]]`. Today the resource module allows only instance-local (`+`) properties to change at runtime, for good reasons. Both remove (at start) and add (at stop) would need exceptions to that rule.
3. **Stop must restore state that the job-manager does not own.** To put `batch` back, the job-manager must know which ranks had `batch` before any runtime queue existed. That is part of R, which the resource module owns. We would need new state or interfaces, or a cached copy of R in the job-manager that can go stale.
4. **The job-manager cannot always tell which property is a queue's membership.** A queue's `requires` is a general constraint. With `["foo|bar"]`, `["^foo"]`, several terms, or no `requires` at all, there is no single property to remove. With `requires = ["gpu"]`, removing the property would strip a hardware property from the nodes.
5. **The job-manager must compute a diff against R.** It fetches R, computes what to add and remove, and sends the result. A concurrent change can make that diff stale, and the resource module does not reject a stale remove.
6. **Order matters when runtime queues overlap.** "Most recently started wins" needs a total order that must persist across restart. Replaying stored deltas after a restart is not idempotent.
7. **Every action recomputes derived state.** Start, stop, remove, restart, config reload, and resource reload all have to rebuild the full membership and push the difference.

Problems 2 to 7 all come from changing the donor. The new mechanism never changes the donor.

### Summary of the new design

- `flux queue add dat hosts=node[0-3]` creates a queue at runtime. The queue starts in the stopped state. Users can submit to it right away.
- The queue's nodes get the property `+dat`. The donor's properties (for example `batch`) never change.
- When the queue is started, `+dat` becomes **exclusive**. The scheduler then gives those nodes only to jobs that require `+dat`. Other jobs can still be *feasible* on those nodes, so they wait instead of failing.
- When the queue is stopped, `+dat` is no longer exclusive. Donor jobs can use the nodes again as soon as the nodes are free.
- `flux queue remove dat` removes the queue and the `+dat` property.
- A configured `hosts=` queue can use the same mechanism with `exclusive = true`.

### Exclusive properties

#### Definitions

- **Property:** an RFC 20 property name on a rank, for example `batch` or `+dat`.
- **Exclusive set (E):** a set of property names, for example `{"+dat"}`. The resource module holds it. It is *not* part of R and is not stored in the KVS.
- **Exclusive rank:** a rank that has at least one property in E.
- **Required properties of a job:** `requires(C)`, where C is the job's constraint (defined below).

#### Scheduler rules

A scheduler that supports exclusive properties follows these rules:

1. **Feasibility ignores E.** This covers the satisfiability check at alloc time and `feasibility.check` at submit. If a job could run on all ranks, it is feasible.
2. **Allocation respects E.** The scheduler may allocate exclusive rank r to job J only if J requires every property in E that r has.
3. **Planning respects E.** Backfill reservations and other future planning follow rule 2. E has no end time, so a job that does not require X cannot plan on a rank with X. The job is not rejected; it waits.
4. **Existing allocations are not affected.** Running jobs and housekeeping keep their ranks. E affects only new allocations.
5. **A change to E triggers a new scheduling pass.** Jobs that were set aside because of E must be considered again.

Drained nodes already follow these rules today: they count for feasibility, but nothing is allocated on them. An exclusive rank behaves like a drained rank for jobs that do not require the exclusive property, and like a normal rank for jobs that do.

#### Which jobs require a property

`requires(C)` is the set of properties that constraint C guarantees a rank must have. It is computed from the structure of C:

| operator | `requires` |
| --- | --- |
| `properties: [...]` | the names without `^` |
| `and: [...]` | union of the children |
| `or: [...]` | intersection of the children |
| `not: [...]` | empty set |
| no constraint | empty set |

Examples:

| constraint | `requires` | may use exclusive `+dat` ranks? |
| --- | --- | --- |
| `{"properties": ["batch"]}` | `{batch}` | no |
| `{"properties": ["+dat"]}` | `{+dat}` | yes |
| `{"properties": ["+dat", "batch"]}` | `{+dat, batch}` | yes |
| `{"or": [{"properties": ["+dat"]}, {"properties": ["batch"]}]}` | `{}` | no |
| `{"or": [{"properties": ["+dat", "a"]}, {"properties": ["+dat", "b"]}]}` | `{+dat}` | yes |
| no constraint | `{}` | no |

The rule is computed once per job, not per rank. It does not depend on the exact shape that the frobnicator produces. For any constraint the frobnicator produces, it gives the expected answer.

The frobnicator adds each queue's `requires` to its jobs. Every job in queue `dat` therefore requires `+dat`. A virtual queue whose parent is `dat` inherits `requires`, so its jobs require `+dat` too.

#### Access rule

Rule 2 trusts the job's constraint. Without another check, a `batch` user could add `--requires=+dat` and run on the DAT nodes.

Rule: **a job may not name the property of an exclusive queue it is not in.** "Exclusive queue" here means any runtime queue, and any configured queue with `exclusive = true`. The job-manager rejects such a job at submit. It applies the same check when `flux update` moves a job to another queue. A job may still name `^+dat` to avoid those nodes.

#### Example

Setup:

| ranks | properties in R |
| --- | --- |
| node\[0-3\] | `batch`, `+dat` |
| node\[4-15\] | `batch` |

| time | event | E | effect |
| --- | --- | --- | --- |
| Mon | `flux queue add dat hosts=node[0-3]` | `{}` | `+dat` added; `dat` queue enabled and stopped |
| Mon–Tue | users submit to `dat` | `{}` | jobs wait (queue stopped); `batch` jobs use all 16 nodes |
| Tue 8am | `flux queue start dat` | `{+dat}` | new `batch` jobs use only node\[4-15\]; `dat` jobs use node\[0-3\] as they become free |
| during | 16-node `batch` job pending | `{+dat}` | feasible, so not rejected; waits |
| end | `flux queue stop dat` | `{}` | `batch` jobs can use node\[0-3\] as they become free; the 16-node job can run |
| later | `flux queue remove dat` | `{}` | `+dat` removed from R |

Running `batch` jobs on node\[0-3\] at 8am keep running. When they finish, only `dat` jobs can use those nodes.

#### Protocol

The names below are proposals.

- **`resource.property-update`** (job-manager to resource module) and **`sched.property-update`** (resource module to scheduler) gain two optional keys, `exclusive-add` and `exclusive-remove`. Each is a list of property names. As today, all keys in one request are applied together or not at all. The resource module applies the change only after the scheduler confirms it.
- The **initial `resource.acquire` response** gains `exclusive`, the current E. A scheduler that loads or reloads therefore gets E without help from the job-manager.
- The **`resource.acquire` request** gains a capability flag, for example `"exclusive": true`. If a scheduler does not set it:
  - the resource module rejects exclusive updates while that scheduler is attached;
  - it refuses the acquire request if E is not empty.

  This makes the system fail closed. A scheduler that does not understand E cannot silently ignore it.
- Only `+` properties may be in E.

#### Order of operations

- **Start:** `exclusive-add`, then wait for the scheduler to confirm, then start the queue. When the queue's jobs are first considered, other jobs are already excluded.
- **Stop:** stop the queue (pending allocation requests for the queue are canceled), then `exclusive-remove`. No new job from the queue starts after the nodes are returned.

#### Restart and reload

- **Full restart:** R is rebuilt without runtime properties. The job-manager restores runtime queues from its checkpoint before it loads jobs. It then sends one update before the scheduler loads; module load order is resource, then job-manager, then scheduler. The update has `property-add` for every runtime queue, and `exclusive-add` for every started exclusive queue, runtime or configured. Every operation is a set union, so the update is idempotent.
- **Scheduler reload:** the scheduler gets the properties (in R) and E from the initial acquire response. No other action is needed.
- **Job-manager reload:** same as a full restart. The update changes nothing, because the state is already correct.
- **Resource module reload:** the runtime properties and E are lost. In the MVP, reload the job-manager after the resource module.

#### Implementation notes for schedulers

- Do not implement E by rewriting each job's constraint, for example by adding `^+dat`. That adds work for every job that does not use the reserved nodes, which is most jobs.
- Compute an opt-in set per job from `requires()`, once. Tag each exclusive rank. At allocation, skip a rank if its tags are not a subset of the job's opt-in set.
- **Fluxion:** add the check next to `by_status()` in `dfu_impl_t::prune()`. Skip it for `AT_SATISFIABILITY`, as `by_status()` does. Backfill policies already move jobs that cannot reserve to the blocked list. They already reconsider those jobs on resource notify, so a change to E must send a notify.
- **Python scheduler:** filter candidate ranks at allocation. Leave the feasibility check unchanged.
- **Backfill behind a permanently unfittable head job.** `sched-backfill` gates all backfill on having a shadow time for the head job, and `_shadow_time()` only advances a simulated pool through job expirations. A head job that still does not fit once every running job has ended therefore gets no shadow, and nothing behind it backfills. Exclusive ranks produce such a head job -- but so do drained ranks, and measurements on a real instance show the two behave identically. This is pre-existing EASY-backfill behavior, not something E introduces, and it needs no time bound on E to fix. Normal traffic is unaffected: with a runtime queue started, its jobs and the donor queue's jobs both flow at full rate. See `RUNTIME-QUEUES-BACKFILL-FINDINGS.md` for the measurements, two false leads, and a reproducer.

### Exclusive configured queues

The same mechanism works for configured queues:

```toml
[queues.debug]
hosts = "node[0-3]"
exclusive = true
```

`+debug` is applied to node\[0-3\] at resource module load, as today. While `debug` is started, `+debug` is in E. While it is stopped, `batch` jobs can use node\[0-3\].

Only one queue is actively scheduling on those nodes at any time. This avoids the fairness problems of overlapping queues. Example uses are a daytime debug queue that cron starts and stops, or a weekly DAT.

Rules:

- `exclusive` requires `hosts`, because only `+` properties may be in E.
- Exclusive queues may not overlap each other or any runtime queue. Otherwise a rank could have two exclusive properties, and no job would qualify for it under rule 2.
- An exclusive queue starts stopped the first time it is configured. After that, its state is saved like any other queue.
- If a config reload changes `exclusive` on a started queue, or removes a started exclusive queue, the job-manager updates E.
- Changing `hosts` still needs a resource module reload, as today.
- The access rule applies to exclusive configured queues.

### Where state lives

- **job-manager:** owns the queue set. Stores runtime queues in its checkpoint. Decides when properties and E change.
- **resource module:** owns R and E. Adds `+NAME` to the R it serves. Sends R and E to the scheduler and waits for confirmation. Stores neither in the KVS.
- **scheduler:** applies properties and E. Responds only after the change is in effect.

### Lifecycle (runtime queues)

| action | steps (job-manager) |
| --- | --- |
| add | validate, checkpoint, `property-add +dat`, `sched.queue-update`, create queue (enabled, stopped), notify consumers |
| start | `exclusive-add +dat` and wait for the scheduler, then start the queue |
| stop | stop the queue, then `exclusive-remove +dat` |
| remove | stop, raise exceptions on pending jobs (requires `--force`), `property-remove +dat`, remove the queue from the scheduler, checkpoint |
| restart | restore runtime queues before jobs load, then send one update with every runtime `+NAME` and the E of started exclusive queues |

Exclusive configured queues use the start, stop, and restart rows only. Their property comes from config.

### Alternatives considered

- **Change the donor queue's `requires`** (for example to `batch & ^+dat`). This affects only new jobs. Pending jobs keep the constraint they were submitted with. It also changes feasibility, as in problem 1.
- **Reservations as jobs, or a subinstance per DAT.** These are possible future directions; see Future work.

### MVP limits

- `hosts` is a hostlist. Exclusive queues may not overlap each other; this covers runtime queues and configured queues with `exclusive = true`.
- Start and stop are manual (`flux queue start/stop`), or run by a script at the planned time.
- There is no lookahead. Jobs that are still running on the nodes at start time keep running. The exclusive queue waits for them.
- A large donor job that cannot fit while an exclusive queue is started used to block **every** queue, starving the exclusive queue's own jobs while its nodes sat idle. That was pre-existing scheduler behavior rather than anything E introduced -- two ordinary configured queues on disjoint nodes starved each other the same way -- and it is now fixed in both Python schedulers: `sched-simple` marks only the blocked job's own queue blocked for the pass, and `sched-backfill` drops the EASY constraints when the head job has no reservation to protect. A large donor job now waits in its own queue without holding up the others. Whether Fluxion needs the equivalent change is still open. See `RUNTIME-QUEUES-STARVATION-FINDING.md`.
- A reload of the resource module loses runtime properties and E. Reload the job-manager after it.
- Fluxion sets match-policy for the whole instance, so `sched.*` can set the queue-policy and queue parameters only, not match-policy per queue.
- No `flux queue update`, and no runtime virtual queues (`--parent`).

### Future work

- Scheduled start and end times.
- Scheduler lookahead, so that nodes are free at the start time. Fluxion reservations could provide this. A time bound on E would also let large donor jobs plan for after the exclusive period ends.
- Exclusive queues that overlap each other, with a rule for ranks that have more than one exclusive property.
- An eventlog for audit.

### RFC changes

- **RFC 28** (resource acquisition):
  - E in the initial `resource.acquire` response;
  - `exclusive-add` and `exclusive-remove` in `sched.property-update`;
  - the scheduler capability flag;
  - scheduler rules 1 to 5.
- **RFC 31** (constraints): define `requires()`.
- **RFC 27** (allocation): add `sched.queue-update` to add and remove queues at runtime, with scheduler options. A draft is on branch `rfc27-queue-update`.
- **RFC 33** (queues):
  - runtime queues: create, remove, persistence, and exclusive use while started;
  - the `exclusive` key for configured queues;
  - the access rule;
  - how runtime queues interact with configured queues and config reload.
- **RFC 20**: no change. E is not part of R.

### TODO (MVP)

**resource module**

- [x] `resource.property-update` (`property-add`, `property-remove`), with scheduler confirmation
- [ ] `exclusive-add` and `exclusive-remove` in `resource.property-update` and `sched.property-update`
- [ ] E in the initial acquire response
- [ ] scheduler capability flag; reject exclusive updates, and refuse acquire when E is not empty, for schedulers without it
- [ ] build the trial R from the current R at commit, and return `EBUSY` while an update is pending

**Python scheduler**

- [x] `sched.property-update` for `property-add` and `property-remove`
- [ ] E: exclusive filter at allocation (feasibility unchanged)
- [ ] `requires()` helper in the Python bindings, shared with tests
- [ ] support for `sched.queue-update`, if needed for tests

**job-manager**

- [ ] `job-manager.queue-add` and `job-manager.queue-remove` RPCs
- [ ] validate hosts, name conflicts, and overlap with other exclusive queues
- [ ] access rule: reject jobs that name another exclusive queue's property, at submit and on queue update
- [ ] write-ahead checkpoint in a new optional key (no version bump)
- [ ] restore runtime queues before jobs load
- [ ] push properties and E at startup, before the scheduler connects; hold `sched-ready` if R is not ready
- [ ] start and stop ordering for exclusive queues
- [ ] remove: raise exceptions on pending jobs; require `--force`
- [ ] config reload: static config plus runtime queues; conflicts are errors
- [ ] send `sched.queue-update` on add and remove, and again after scheduler hello
- [ ] jobtap queues-update notification on add and remove
- [ ] streaming `job-manager.queue-list`

**configured exclusive queues**

- [ ] `exclusive` key in `[queues.NAME]`, valid only with `hosts`
- [ ] validation: no overlap between exclusive queues, configured or runtime
- [ ] initial stopped state for new exclusive queues
- [ ] config reload: update E when `exclusive` changes or a started exclusive queue is removed

**job-ingest**

- [ ] refresh frobnicator and validator queue config when queues change

**CLI**

- [ ] `flux queue add NAME hosts=HOSTLIST [key=value...]`, with `sched.*` passed through to the scheduler
- [ ] `flux queue remove NAME [--force]`
- [ ] `policy.*` keys (if time allows)

**docs and tests**

- [ ] sharness tests:
  - runtime queue: add, submit before start, start, stop, remove
  - restart, and a reload of only the job-manager
  - large donor job stays pending
  - access rule
  - submit from a non-zero rank right after add
  - exclusive configured queue: start, stop, config reload, restart
- [ ] admin guide section on dedicated access with runtime queues and exclusive configured queues
- [ ] RFC updates listed above

**Fluxion** (flux-framework/flux-sched, tracked separately)

- [ ] `sched.property-update` with properties and E
- [ ] exclusive tag on node vertices, checked next to `by_status()`
- [ ] opt-in mask computed once per job from `requires()`
- [ ] notify qmanager when E changes, so blocked jobs are reconsidered
- [ ] `sched.queue-update`: create and remove qmanager queues at runtime
- [ ] capability flag in `resource.acquire`
- [ ] omit `+` properties from job R (existing bug)%
