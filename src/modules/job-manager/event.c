/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* event.c - job state machine and eventlog commit batching
 *
 * event_job_update() implements the job state machine described
 * in RFC 21.  This function is called when an event occurs for a job,
 * to drive changes to job state and flags.  For example, an "alloc"
 * event transitions a job from SCHED to RUN state.
 *
 * event_job_action() is called after event_job_update().  It takes actions
 * appropriate for job state and flags.  For example, in RUN state,
 * job shells are started.
 *
 * Events are logged in the job eventlog in the KVS.  For performance,
 * multiple updates may be combined into one commit.  The location of
 * the job eventlog and its contents are described in RFC 16 and RFC 18.
 *
 * The function event_job_post_pack() posts an event to a job, running
 * event_job_update(), event_job_action(), and committing the event to
 * the job eventlog, in a delayed batch.
 *
 * Notes:
 * - A KVS commit failure is handled as fatal to the job-manager
 * - event_job_action() is idempotent
 * - event_ctx_destroy() flushes batched eventlog updates before returning
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <czmq.h>
#include <jansson.h>
#include <flux/core.h>

#include "alloc.h"
#include "start.h"
#include "drain.h"
#include "journal.h"
#include "wait.h"

#include "event.h"

#include "src/common/libeventlog/eventlog.h"

const double batch_timeout = 0.01;

struct event {
    struct job_manager *ctx;
    struct event_batch *batch;
    flux_watcher_t *timer;
    zlist_t *pending;
    zlist_t *pub_futures;
};

struct event_batch {
    struct event *event;
    flux_kvs_txn_t *txn;
    flux_future_t *f;
    json_t *state_trans;
    zlist_t *responses; // responses deferred until batch complete
};

static struct event_batch *event_batch_create (struct event *event);
static void event_batch_destroy (struct event_batch *batch);

/* Batch commit has completed.
 * If there was a commit error, log it and stop the reactor.
 * Destroy 'batch'.
 */
static void commit_continuation (flux_future_t *f, void *arg)
{
    struct event_batch *batch = arg;
    struct event *event = batch->event;
    struct job_manager *ctx = event->ctx;

    if (flux_future_get (batch->f, NULL) < 0) {
        flux_log_error (ctx->h, "%s: eventlog update failed", __FUNCTION__);
        flux_reactor_stop_error (flux_get_reactor (ctx->h));
    }
    zlist_remove (event->pending, batch);
    event_batch_destroy (batch);
}

/* job-state event publish has completed.
 * If there was a publish error, log it and stop the reactor.
 * Destroy 'f'.
 */
static void publish_continuation (flux_future_t *f, void *arg)
{
    struct event *event = arg;
    struct job_manager *ctx = event->ctx;

    if (flux_future_get (f, NULL) < 0) {
        flux_log_error (ctx->h, "%s: event publish failed", __FUNCTION__);
        flux_reactor_stop_error (flux_get_reactor (ctx->h));
    }
    zlist_remove (event->pub_futures, f);
    flux_future_destroy (f);
}

/* Close the current batch, if any, and commit it.
 */
static void event_batch_commit (struct event *event)
{
    struct event_batch *batch = event->batch;
    struct job_manager *ctx = event->ctx;

    if (batch) {
        event->batch = NULL;
        /* note that job-state events will be sent after the KVS
         * commit, as we want to ensure anyone who receives a
         * job-state transition event will be able to read the
         * corresponding event in the KVS.
         */
        if (batch->txn) {
            if (!(batch->f = flux_kvs_commit (ctx->h, NULL, 0, batch->txn)))
                goto error;
            if (flux_future_then (batch->f, -1., commit_continuation, batch) < 0)
                goto error;
            if (zlist_append (event->pending, batch) < 0)
                goto nomem;
        }
        else { // just publish events & send responses and be done
            event_batch_destroy (batch);
        }
    }
    return;
nomem:
    errno = ENOMEM;
error: // unlikely (e.g. ENOMEM)
    flux_log_error (ctx->h, "%s: aborting reactor", __FUNCTION__);
    flux_reactor_stop_error (flux_get_reactor (ctx->h));
    event_batch_destroy (batch);
}

static void timer_cb (flux_reactor_t *r, flux_watcher_t *w, int revents, void *arg)
{
    struct job_manager *ctx = arg;
    event_batch_commit (ctx->event);
}

void event_publish (struct event *event, const char *topic,
                    const char *key, json_t *o)
{
    struct job_manager *ctx = event->ctx;
    flux_future_t *f;

    if (!(f = flux_event_publish_pack (ctx->h, topic, 0, "{s:O?}", key, o))) {
        flux_log_error (ctx->h, "%s: flux_event_publish_pack", __FUNCTION__);
        goto error;
    }
    if (flux_future_then (f, -1., publish_continuation, event) < 0) {
        flux_future_destroy (f);
        flux_log_error (ctx->h, "%s: flux_future_then", __FUNCTION__);
        goto error;
    }
    if (zlist_append (event->pub_futures, f) < 0) {
        flux_future_destroy (f);
        flux_log_error (ctx->h, "%s: zlist_append", __FUNCTION__);
        goto error;
    }
    return;
error:
    flux_reactor_stop_error (flux_get_reactor (ctx->h));
}

/* Besides cleaning up, this function has the following side effects:
 * - publish state transition event (if any)
 * - send listener responses (only under error scenarios, should be
 *   sent in event_batch_commit()).
 * - respond to deferred responses (if any)
 */
static void event_batch_destroy (struct event_batch *batch)
{
    if (batch) {
        int saved_errno = errno;

        flux_kvs_txn_destroy (batch->txn);
        if (batch->f)
            (void)flux_future_wait_for (batch->f, -1);
        if (batch->state_trans) {
            if (json_array_size (batch->state_trans) > 0)
                event_publish (batch->event,
                               "job-state",
                               "transitions",
                               batch->state_trans);
            json_decref (batch->state_trans);
        }
        if (batch->responses) {
            flux_msg_t *msg;
            flux_t *h = batch->event->ctx->h;
            while ((msg = zlist_pop (batch->responses))) {
                if (flux_send (h, msg, 0) < 0)
                    flux_log_error (h, "error sending batch response");
                flux_msg_decref (msg);
            }
            zlist_destroy (&batch->responses);
        }
        flux_future_destroy (batch->f);
        free (batch);
        errno = saved_errno;
    }
}

static struct event_batch *event_batch_create (struct event *event)
{
    struct event_batch *batch;

    if (!(batch = calloc (1, sizeof (*batch))))
        return NULL;
    batch->event = event;
    return batch;
}

/* Create a new "batch" if there is none.
 * No-op if batch already started.
 */
static int event_batch_start (struct event *event)
{
    if (!event->batch) {
        if (!(event->batch = event_batch_create (event)))
            return -1;
        flux_timer_watcher_reset (event->timer, batch_timeout, 0.);
        flux_watcher_start (event->timer);
    }
    return 0;
}

static int event_batch_commit_event (struct event *event,
                                     struct job *job,
                                     json_t *entry)
{
    char key[64];
    char *entrystr = NULL;

    if (event_batch_start (event) < 0)
        return -1;
    if (flux_job_kvs_key (key, sizeof (key), job->id, "eventlog") < 0)
        return -1;
    if (!event->batch->txn && !(event->batch->txn = flux_kvs_txn_create ()))
        return -1;
    if (!(entrystr = eventlog_entry_encode (entry)))
        return -1;
    if (flux_kvs_txn_put (event->batch->txn,
                          FLUX_KVS_APPEND,
                          key,
                          entrystr) < 0) {
        free (entrystr);
        return -1;
    }
    free (entrystr);
    return 0;
}

int event_batch_pub_state (struct event *event, struct job *job,
                           double timestamp)
{
    json_t *o;

    if (event_batch_start (event) < 0)
        goto error;
    if (!event->batch->state_trans) {
        if (!(event->batch->state_trans = json_array ()))
            goto nomem;
    }
    if (!(o = json_pack ("[I,s,f]",
                         job->id,
                         flux_job_statetostr (job->state, false),
                         timestamp)))
        goto nomem;
    if (json_array_append_new (event->batch->state_trans, o)) {
        json_decref (o);
        goto nomem;
    }
    return 0;
nomem:
    errno = ENOMEM;
error:
    return -1;
}

int event_batch_respond (struct event *event, const flux_msg_t *msg)
{
    if (event_batch_start (event) < 0)
        return -1;
    if (!event->batch->responses) {
        if (!(event->batch->responses = zlist_new ()))
            goto nomem;
    }
    if (zlist_append (event->batch->responses,
                      (void *)flux_msg_incref (msg)) < 0) {
        flux_msg_decref (msg);
        goto nomem;
    }
    return 0;
nomem:
    errno = ENOMEM;
    return -1;
}

int event_job_action (struct event *event, struct job *job)
{
    struct job_manager *ctx = event->ctx;

    switch (job->state) {
        case FLUX_JOB_STATE_NEW:
            break;
        case FLUX_JOB_STATE_DEPEND:
            if (event_job_post_pack (event, job, "depend", 0, NULL) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_PRIORITY:
            /* N.B. Priority will be set via a priority plugin call in
             * the future. For the time being, we pass the
             * urgency set via submit or urgency change.
             *
             * In the event we have re-entered this state from the
             * SCHED state, dequeue the job first.
             */
            alloc_dequeue_alloc_request (ctx->alloc, job);
            if (event_job_post_pack (event,
                                     job,
                                     "priority",
                                     0,
                                     "{ s:i }",
                                     "priority", job->urgency) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_SCHED:
            if (alloc_enqueue_alloc_request (ctx->alloc, job) < 0)
                return -1;
            if (alloc_queue_recalc_pending (ctx->alloc) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_RUN:
            if (start_send_request (ctx->start, job) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_CLEANUP:
            if (job->alloc_pending)
                alloc_cancel_alloc_request (ctx->alloc, job);
            if (job->alloc_queued)
                alloc_dequeue_alloc_request (ctx->alloc, job);

            /* N.B. start_pending indicates that the start request is still
             * expecting responses.  The final response is the 'release'
             * response with final=true.  Thus once the flag is clear,
             * it is safe to release all resources to the scheduler.
             */
            if (job->has_resources && !job->start_pending
                                   && !job->free_pending) {
                if (alloc_send_free_request (ctx->alloc, job) < 0)
                    return -1;
            }
            /* Post cleanup event when cleanup is complete.
             */
            if (!job->alloc_queued && !job->alloc_pending
                                   && !job->free_pending
                                   && !job->start_pending
                                   && !job->has_resources) {

                if (event_job_post_pack (event, job, "clean", 0, NULL) < 0)
                    return -1;
            }
            break;
        case FLUX_JOB_STATE_INACTIVE:
            if ((job->flags & FLUX_JOB_WAITABLE))
                wait_notify_inactive (ctx->wait, job);
            zhashx_delete (ctx->active_jobs, &job->id);
            drain_check (ctx->drain);
            break;
    }
    return 0;
}

static int event_submit_context_decode (json_t *context,
                                        int *urgency,
                                        uint32_t *userid,
                                        int *flags)
{
    if (json_unpack (context, "{ s:i s:i s:i }",
                     "urgency", urgency,
                     "userid", userid,
                     "flags", flags) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_priority_context_decode (json_t *context,
                                          unsigned int *priority)
{
    /* N.B. eventually this will be the priority, but is the
     * same of the urgency at the moment */
    if (json_unpack (context, "{ s:i }", "priority", (int *)priority) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_urgency_context_decode (json_t *context,
                                         int *urgency)
{
    if (json_unpack (context, "{ s:i }", "urgency", urgency) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_exception_context_decode (json_t *context,
                                           int *severity)
{
    if (json_unpack (context, "{ s:i }", "severity", severity) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_release_context_decode (json_t *context,
                                         int *final)
{
    *final = 0;

    if (json_unpack (context, "{ s:b }", "final", &final) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

/* This function implements state transitions per RFC 21.
 * If FLUX_JOB_WAITABLE flag is set, then on a fatal exception or
 * cleanup event, capture the event in job->end_event for flux_job_wait().
 */
int event_job_update (struct job *job, json_t *event)
{
    double timestamp;
    const char *name;
    json_t *context;

    if (eventlog_entry_parse (event, &timestamp, &name, &context) < 0)
        goto error;

    if (!strcmp (name, "submit")) {
        if (job->state != FLUX_JOB_STATE_NEW)
            goto inval;
        job->t_submit = timestamp;
        if (event_submit_context_decode (context,
                                         &job->urgency,
                                         &job->userid,
                                         &job->flags) < 0)
            goto error;
        job->state = FLUX_JOB_STATE_DEPEND;
    }
    if (!strcmp (name, "depend")) {
        if (job->state != FLUX_JOB_STATE_DEPEND)
            goto inval;
        job->state = FLUX_JOB_STATE_PRIORITY;
    }
    else if (!strcmp (name, "priority")) {
        if (job->state != FLUX_JOB_STATE_PRIORITY
            && job->state != FLUX_JOB_STATE_SCHED)
            goto inval;
        if (event_priority_context_decode (context, &job->priority) < 0)
            goto error;
        job->state = FLUX_JOB_STATE_SCHED;
    }
    else if (!strcmp (name, "urgency")) {
        if (event_urgency_context_decode (context, &job->urgency) < 0)
            goto error;
    }
    else if (!strcmp (name, "exception")) {
        int severity;
        if (job->state == FLUX_JOB_STATE_NEW
            || job->state == FLUX_JOB_STATE_INACTIVE)
            goto inval;
        if (event_exception_context_decode (context, &severity) < 0)
            goto error;
        if (severity == 0) {
            if ((job->flags & FLUX_JOB_WAITABLE) && !job->end_event)
                job->end_event = json_incref (event);

            job->state = FLUX_JOB_STATE_CLEANUP;
        }
    }
    else if (!strcmp (name, "alloc")) {
        if (job->state != FLUX_JOB_STATE_SCHED
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        job->has_resources = 1;
        if (job->state == FLUX_JOB_STATE_SCHED)
            job->state = FLUX_JOB_STATE_RUN;
    }
    else if (!strcmp (name, "free")) {
        if (job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        job->has_resources = 0;
    }
    else if (!strcmp (name, "finish")) {
        if (job->state != FLUX_JOB_STATE_RUN
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        if (job->state == FLUX_JOB_STATE_RUN) {
            if ((job->flags & FLUX_JOB_WAITABLE) && !job->end_event)
                job->end_event = json_incref (event);

            job->state = FLUX_JOB_STATE_CLEANUP;
        }
    }
    else if (!strcmp (name, "release")) {
        int final;
        if (job->state != FLUX_JOB_STATE_RUN
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        if (event_release_context_decode (context, &final) < 0)
            goto error;
        if (final && job->state == FLUX_JOB_STATE_RUN)
            goto inval;
    }
    else if (!strcmp (name, "clean")) {
        if (job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        job->state = FLUX_JOB_STATE_INACTIVE;
    }
    else if (!strcmp (name, "flux-restart")) {
        /* The flux-restart event is currently only posted to jobs in
         * SCHED state since that is the only state transition defined
         * for the event in RFC21.  In the future, other transitions
         * may be defined.
         */
        if (job->state == FLUX_JOB_STATE_SCHED)
            job->state = FLUX_JOB_STATE_PRIORITY;
    }
    return 0;
inval:
    errno = EINVAL;
error:
    return -1;
}

static int get_timestamp_now (double *timestamp)
{
    struct timespec ts;
    if (clock_gettime (CLOCK_REALTIME, &ts) < 0)
        return -1;
    *timestamp = (1E-9 * ts.tv_nsec) + ts.tv_sec;
    return 0;
}

int event_job_post_pack (struct event *event,
                         struct job *job,
                         const char *name,
                         int flags,
                         const char *context_fmt,
                         ...)
{
    va_list ap;
    json_t *entry = NULL;
    int saved_errno;
    double timestamp;
    flux_job_state_t old_state = job->state;
    int eventlog_seq = (flags & EVENT_JOURNAL_ONLY) ? -1 : job->eventlog_seq;

    va_start (ap, context_fmt);
    if (get_timestamp_now (&timestamp) < 0)
        goto error;
    if (!(entry = eventlog_entry_vpack (timestamp, name, context_fmt, ap)))
        return -1;
    /* call before eventlog_seq increment below */
    if (journal_process_event (event->ctx->journal,
                               job->id,
                               eventlog_seq,
                               name,
                               entry) < 0)
        goto error;
    if ((flags & EVENT_JOURNAL_ONLY))
        goto out;
    if (event_job_update (job, entry) < 0) // modifies job->state
        goto error;
    job->eventlog_seq++;
    if (event_batch_commit_event (event, job, entry) < 0)
        goto error;
    if (job->state != old_state) {
        if (event_batch_pub_state (event, job, timestamp) < 0)
            goto error;
    }

    /* Keep track of running job count.
     * If queue reaches idle state, event_job_action() triggers any waiters.
     */
    if ((job->state & FLUX_JOB_STATE_RUNNING)
        && !(old_state & FLUX_JOB_STATE_RUNNING))
        event->ctx->running_jobs++;
    else if (!(job->state & FLUX_JOB_STATE_RUNNING)
             && (old_state & FLUX_JOB_STATE_RUNNING))
        event->ctx->running_jobs--;

    if (event_job_action (event, job) < 0)
        goto error;

out:
    json_decref (entry);
    va_end (ap);
    return 0;
error:
    saved_errno = errno;
    json_decref (entry);
    va_end (ap);
    errno = saved_errno;
    return -1;
}

/* Finalizes in-flight batch KVS commits and event pubs (synchronously).
 */
void event_ctx_destroy (struct event *event)
{
    if (event) {
        int saved_errno = errno;
        flux_watcher_destroy (event->timer);
        event_batch_commit (event);
        if (event->pending) {
            struct event_batch *batch;
            while ((batch = zlist_pop (event->pending)))
                event_batch_destroy (batch); // N.B. can append to pub_futures
        }
        zlist_destroy (&event->pending);
        if (event->pub_futures) {
            flux_future_t *f;
            while ((f = zlist_pop (event->pub_futures))) {
                if (flux_future_get (f, NULL) < 0)
                    flux_log_error (event->ctx->h,
                                    "error publishing job-state event");
                flux_future_destroy (f);
            }
        }
        zlist_destroy (&event->pub_futures);
        free (event);
        errno = saved_errno;
    }
}

struct event *event_ctx_create (struct job_manager *ctx)
{
    struct event *event;

    if (!(event = calloc (1, sizeof (*event))))
        return NULL;
    event->ctx = ctx;
    if (!(event->timer = flux_timer_watcher_create (flux_get_reactor (ctx->h),
                                                    0.,
                                                    0.,
                                                    timer_cb,
                                                    ctx)))
        goto error;
    if (!(event->pending = zlist_new ()))
        goto nomem;
    if (!(event->pub_futures = zlist_new ()))
        goto nomem;

    return event;
nomem:
    errno = ENOMEM;
error:
    event_ctx_destroy (event);
    return NULL;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

