/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* queue.c - job queue service layer
 *
 * This is the service layer for job queues.  All queue-table state is
 * delegated to struct queues (queues.[ch]).  This file owns:
 *
 * - struct queue_ctx: job_manager back-pointer, msg handlers, queues
 *   object, and stop_on_restart flag.
 * - Four RPC callbacks: queue-list, queue-status, queue-enable, queue-start.
 * - queue_configure/queue_rollback: flux_conf_t glue.
 * - enqueue_jobs/dequeue_jobs: driven from the change-notification callback.
 * - queue_submit_check: submission gate.
 * - queue_started: alloc helper.
 * - queue_ctx_save/restore: checkpoint delegation.
 * - .update-queue jobtap plugin.
 *
 * Notes:
 * - By default, only a single anonymous queue is defined.  If any named
 *   queues are defined, the anonymous queue is removed.
 *
 * - A job requests to be in a particular queue by requiring the resource
 *   property associated with the nodes in the queue.  If it requires nothing,
 *   the anonymous queue is assumed.  The 'default' frobnicator plugin may be
 *   configured to add a default queue name when one is unspecified.
 *
 * - When a queue is enabled, jobs submitted for that queue are accepted.
 *   When it is disabled, the job submission program fails immediately.
 *
 * - When a queue is started, alloc requests for jobs in SCHED state are
 *   presented to the scheduler.  When it is stopped, those alloc requests
 *   are canceled.
 *
 * - After a queue is stopped, the job manager continues to send free
 *   requests to the scheduler for the queue as resources are released.
 *   Jobs/housekeeping are not canceled when a queue is stopped.
 *
 * - When a queue is enabled and stopped, job submissions to the queue are
 *   accepted, but the jobs will not run until the queue is started.
 *
 * See also:
 * RFC 33/Flux Job Queues
 * RFC 27/Resource Allocation Protocol Version 1
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>

#include "src/common/libutil/errprintf.h"
#include "src/common/libutil/jpath.h"
#include "src/common/libutil/errno_safe.h"
#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libfluxutil/conf_policy.h"
#include "ccan/str/str.h"

#include "alloc.h"
#include "job-manager.h"
#include "jobtap-internal.h"
#include "jobtap.h"
#include "conf.h"
#include "restart.h"
#include "queues.h"
#include "queue.h"
#include "runtime.h"

struct queue_ctx {
    struct job_manager *ctx;
    flux_msg_handler_t **handlers;
    struct queues *queues;
    bool stop_on_restart;           // stop started queues on restart
    struct queues *saved;           // pre-configure copy, for rollback
    bool saved_stop_on_restart;
    bool busy;                      // a runtime queue operation is in flight
};

static int enqueue_jobs (struct queue_ctx *qctx, const char *name);
static void dequeue_jobs (struct queue_ctx *qctx, const char *name);

/* Queue change-notification callback: drives enqueue/dequeue side effects.
 *
 * Events and their actions:
 *  "start"  -> enqueue_jobs (name)
 *  "stop"   -> dequeue_jobs (name)
 *  "remove" -> no dequeue (leave active jobs in removed queue)
 *  all others -> no action
 *
 * Notifications are never fired while queues_restore() replays
 * checkpointed state, so these side effects apply only to live
 * administrative changes. queue_ctx_restore() cancels alloc requests
 * for queues restored in a stopped state.
 */
static void on_queue_change (struct queues *queues,
                             struct queue *q,
                             const char *event,
                             void *arg)
{
    struct queue_ctx *qctx = arg;

    if (streq (event, "start")) {
        if (enqueue_jobs (qctx, queue_name (q)) < 0)
            flux_log_error (qctx->ctx->h,
                            "error enqueueing jobs for started queue");
    }
    else if (streq (event, "stop"))
        dequeue_jobs (qctx, queue_name (q));
}

json_t *queue_ctx_get_conf (struct queue_ctx *qctx)
{
    return queues_get_conf (qctx->queues);
}

/* Apply 'conf' to the queue table, then deliver the resolved effective queue
 * configuration to jobtap plugins. A plugin that rejects it fails the config
 * reload.
 *
 * N.B. the basic queue configuration should have already been validated by
 * policy_validate() so we shouldn't need to produce detailed configuration
 * errors for users here.
 */
static int queue_apply (struct queue_ctx *qctx,
                        const flux_conf_t *conf,
                        flux_error_t *error)
{
    json_t *config = NULL;
    json_t *policy = NULL;

    /* N.B. all keys are optional, so unpack fails only on an internal
     * error such as out of memory.
     */
    if (flux_conf_unpack (conf,
                          error,
                          "{s?{s?b} s?o s?o}",
                          "job-manager",
                            "stop-queues-on-restart",
                              &qctx->stop_on_restart,
                          "queues", &config,
                          "policy", &policy) < 0)
        return -1;
    if (queues_configure (qctx->queues, config, error) < 0)
        return -1;
    /* Set after queues_configure() so a config that fails validation does
     * not update the global policy. Borrowed from 'conf'; the queues object
     * increfs it.
     */
    queues_set_global_policy (qctx->queues, policy);
    /* Now that all queue mutations are settled, deliver the resolved
     * effective queue configuration to jobtap plugins.
     */
    return jobtap_notify_queues_update (qctx->ctx->jobtap,
                                        queue_ctx_get_conf (qctx),
                                        error);
}

/* conf.c rollback callback: put back the queue table as it was before the
 * reload, and tell plugins about it.
 *
 * The copy is restored rather than replaying 'conf' (which is unused here)
 * because queue_configure() is not a pure function of the configuration:
 * a replay would restore a removed queue but reset its administrative state
 * - the enable/start bits and operator reasons - to defaults.
 *
 * N.B. conf.c rolls back only callbacks at or before the one that failed,
 * so if this runs, queue_configure() ran in the same reload and 'saved' is
 * that reload's pre-mutation copy, never a stale one.
 */
static int queue_rollback (const flux_conf_t *conf,
                           flux_error_t *error,
                           void *arg)
{
    struct queue_ctx *qctx = arg;

    /* A NULL 'saved' means queues_copy() failed at the top of
     * queue_configure(), before anything was mutated, so there is nothing
     * to undo.
     */
    if (!qctx->saved)
        return 0;
    queues_set (qctx->queues, &qctx->saved);
    qctx->stop_on_restart = qctx->saved_stop_on_restart;
    /* Restore the table before notifying, since the configuration delivered
     * to plugins is read back from it. Unlike queue_apply(), plugins are not
     * offered the change first, because a plugin cannot reject a rollback.
     *
     * Notifying every plugin also unwinds any that accepted the rejected
     * configuration before another plugin refused it. If this fails, those
     * plugins keep the rejected configuration and conf.c can only log it.
     */
    return jobtap_notify_queues_update (qctx->ctx->jobtap,
                                        queue_ctx_get_conf (qctx),
                                        error);
}

static int queue_configure (const flux_conf_t *conf,
                            flux_error_t *error,
                            void *arg)
{
    struct queue_ctx *qctx = arg;

    /* Copy the table before mutating it, for queue_rollback(). Replaced on
     * each configure so that a rollback restores the state as of the reload
     * being undone.
     */
    queues_destroy (qctx->saved);
    if (!(qctx->saved = queues_copy (qctx->queues))) {
        errprintf (error,
                   "error copying queue state: %s",
                   strerror (errno));
        return -1;
    }
    qctx->saved_stop_on_restart = qctx->stop_on_restart;
    if (queue_apply (qctx, conf, error) < 0)
        return -1;
    return 1;
}

static void queue_list_cb (flux_t *h,
                           flux_msg_handler_t *mh,
                           const flux_msg_t *msg,
                           void *arg)
{
    struct queue_ctx *qctx = arg;
    json_t *resp;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        goto error;
    /* resp is a borrowed reference owned by the queues cache; "O"
     * increfs it for the response, so it is not decref'd here.
     */
    if (!(resp = queues_list_response (qctx->queues)))
        goto error;
    if (flux_respond_pack (h, msg, "O", resp) < 0)
        flux_log_error (h, "error responding to job-manager.queue-list");
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "error responding to job-manager.queue-list");
}

static void queue_status_cb (flux_t *h,
                             flux_msg_handler_t *mh,
                             const flux_msg_t *msg,
                             void *arg)
{
    struct queue_ctx *qctx = arg;
    flux_error_t error;
    const char *errmsg = NULL;
    const char *name = NULL;
    struct queue *q;
    json_t *o = NULL;

    if (flux_request_unpack (msg, NULL, "{s?s}", "name", &name) < 0)
        goto error;
    if (!(q = queues_lookup (qctx->queues, name, &error))) {
        errmsg = error.text;
        errno = EINVAL;
        goto error;
    }
    if (!(o = queue_status_encode (q, alloc_sched_ready (qctx->ctx->alloc))))
        goto error;
    if (flux_respond_pack (h, msg, "O", o) < 0)
        flux_log_error (h, "error responding to job-manager.queue-status");
    json_decref (o);
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to job-manager.queue-status");
    json_decref (o);
}

/* This function returns true if an operation on the queue named
 * 'name' (e.g. start or stop) applies to a job submitted to queue
 * 'job_queue'. It is used by enqueue_jobs()/dequeue_jobs() to select
 * jobs.
 *
 * A NULL 'name' applies to every job. Otherwise the operation
 * applies to the job when:
 *  - 'name' is the job's own queue, or
 *  - 'name' is a non-virtual queue and the job's queue is one of its
 *    virtual queues (RFC 33): a virtual queue's jobs are scheduled
 *    as part of the parent queue, so an operation on the parent must
 *    reach them. The converse is not true: an operation on a virtual
 *    queue does not apply to its parent's or sibling queues' jobs.
 *
 * If either name no longer resolves to a configured queue (e.g.
 * removed by a reload - such jobs are intentionally left in place,
 * see the on_queue_change() N.B. above), fall back to an exact name
 * comparison, matching pre-vqueue behavior.
 */
static bool queue_name_covers (struct queue_ctx *qctx,
                               const char *name,
                               const char *job_queue)
{
    struct queue *target;
    struct queue *jq;

    if (!name)
        return true;
    if (!job_queue)
        return false;
    if (!(target = queues_lookup (qctx->queues, name, NULL)))
        return streq (job_queue, name);
    if (!(jq = queues_lookup (qctx->queues, job_queue, NULL)))
        return streq (job_queue, name);
    if (queue_is_virtual (target))
        return jq == target;
    return queue_root (jq) == target;
}

static int enqueue_jobs (struct queue_ctx *qctx, const char *name)
{
    struct job *job = zhashx_first (qctx->ctx->active_jobs);
    while (job) {
        if (queue_name_covers (qctx, name, job->queue)) {
            if (!job->alloc_queued
                && !job->alloc_pending
                && job->state == FLUX_JOB_STATE_SCHED
                && queue_started (qctx, job)) {
                if (alloc_enqueue_alloc_request (qctx->ctx->alloc, job) < 0)
                    return -1;
                if (alloc_queue_recalc_pending (qctx->ctx->alloc) < 0)
                    return -1;
            }
        }
        job = zhashx_next (qctx->ctx->active_jobs);
    }
    return 0;
}

static void dequeue_jobs (struct queue_ctx *qctx, const char *name)
{
    if (alloc_queue_count (qctx->ctx->alloc) > 0
        || alloc_pending_count (qctx->ctx->alloc) > 0) {
        struct job *job = zhashx_first (qctx->ctx->active_jobs);
        while (job) {
            if (queue_name_covers (qctx, name, job->queue)) {
                if (job->alloc_queued)
                    alloc_dequeue_alloc_request (qctx->ctx->alloc, job);
                else if (job->alloc_pending)
                    alloc_cancel_alloc_request (qctx->ctx->alloc, job, false);
            }
            job = zhashx_next (qctx->ctx->active_jobs);
        }
    }
}

/* Release every exclusive queue's property from the resource module's
 * exclusive set, without waiting for a response.
 *
 * Used by an --all stop, which instance shutdown issues and which
 * therefore must not fail.  The queues are already stopped by the time
 * this runs, so the ordering that matters is preserved: no new job from
 * an exclusive queue starts after its nodes are returned.
 */
static void queue_exclusive_release_all (struct queue_ctx *qctx)
{
    flux_t *h = qctx->ctx->h;
    json_t *names;

    if (!(names = queues_exclusive_names (qctx->queues, false))) {
        flux_log_error (h, "error listing exclusive queues");
        return;
    }
    if (json_array_size (names) > 0) {
        flux_future_t *f;
        if (!(f = flux_rpc_pack (h,
                                 "resource.property-update",
                                 0,
                                 FLUX_RPC_NORESPONSE,
                                 "{s:O}",
                                 "exclusive-remove", names)))
            flux_log_error (h, "error releasing exclusive properties");
        flux_future_destroy (f);
    }
    json_decref (names);
}

/* Runtime queue admin operations.
 *
 * Each one follows the same chain:
 *
 *   1. validate;
 *   2. commit the proposed queues.runtime key and wait;
 *   3. send the resource.property-update, if the operation needs one;
 *   4. apply the change to the in-memory queue table;
 *   5. respond.
 *
 * The key is written first so that it never describes less than R does.
 * If step 3 fails, the key is written back from current in-memory state
 * before the original error is returned.  See runtime.c.
 */
enum queue_op {
    QUEUE_OP_ADD,
    QUEUE_OP_REMOVE,
    QUEUE_OP_START,
    QUEUE_OP_STOP,
    QUEUE_OP_ENABLE,
    QUEUE_OP_DISABLE,
};

struct queue_rpc {
    struct queue_ctx *qctx;
    const flux_msg_t *msg;      // the request, answered when complete
    enum queue_op op;
    char *name;
    char *hosts;
    char *reason;
    int nocheckpoint;
};

static const char *queue_op_topic (enum queue_op op)
{
    switch (op) {
        case QUEUE_OP_ADD:
            return "job-manager.queue-add";
        case QUEUE_OP_REMOVE:
            return "job-manager.queue-remove";
        case QUEUE_OP_ENABLE:
        case QUEUE_OP_DISABLE:
            return "job-manager.queue-enable";
        default:
            return "job-manager.queue-start";
    }
}

static void queue_rpc_destroy (struct queue_rpc *qr)
{
    if (qr) {
        int saved_errno = errno;
        if (qr->qctx)
            qr->qctx->busy = false;
        flux_msg_decref (qr->msg);
        free (qr->name);
        free (qr->hosts);
        free (qr->reason);
        free (qr);
        errno = saved_errno;
    }
}

static struct queue_rpc *queue_rpc_create (struct queue_ctx *qctx,
                                           const flux_msg_t *msg,
                                           enum queue_op op,
                                           const char *name,
                                           const char *hosts,
                                           const char *reason)
{
    struct queue_rpc *qr;

    if (!(qr = calloc (1, sizeof (*qr))))
        return NULL;
    qr->qctx = qctx;
    qr->msg = flux_msg_incref (msg);
    qr->op = op;
    if (!(qr->name = strdup (name))
        || (hosts && !(qr->hosts = strdup (hosts)))
        || (reason && !(qr->reason = strdup (reason)))) {
        qr->qctx = NULL;  // not busy yet; do not clear the flag
        queue_rpc_destroy (qr);
        errno = ENOMEM;
        return NULL;
    }
    qctx->busy = true;
    return qr;
}

/* Tell jobtap plugins and job-ingest that the queue table changed, so
 * that a new queue is accepted at submit without a config reload.
 */
static void queue_runtime_notify (struct queue_ctx *qctx)
{
    flux_t *h = qctx->ctx->h;
    flux_error_t error;
    flux_future_t *f;

    if (jobtap_notify_queues_update (qctx->ctx->jobtap,
                                     queue_ctx_get_conf (qctx),
                                     &error) < 0)
        flux_log (h, LOG_ERR, "queues-update: %s", error.text);
    if (!(f = flux_event_publish (h, "job-manager.queue-update", 0, NULL)))
        flux_log_error (h, "error publishing job-manager.queue-update");
    flux_future_destroy (f);
}

/* Write the key back from current in-memory state, after an operation
 * failed with the proposed version already committed.  Best effort: the
 * operation has already failed, so a second failure is only logged.
 */
static void queue_runtime_revert (struct queue_ctx *qctx)
{
    flux_t *h = qctx->ctx->h;
    json_t *o;
    flux_future_t *f = NULL;

    if (!(o = runtime_queues_encode (qctx->queues, NULL, NULL, NULL))
        || !(f = runtime_queues_commit (h, o))
        || flux_future_get (f, NULL) < 0)
        flux_log_error (h, "error reverting %s", RUNTIME_QUEUES_KEY);
    flux_future_destroy (f);
    json_decref (o);
}

/* Apply the operation to the in-memory queue table, now that the key and
 * R both have it.  Returns 0 on success, or -1 with 'errp' set.
 */
static int queue_runtime_apply (struct queue_rpc *qr, flux_error_t *errp)
{
    struct queue_ctx *qctx = qr->qctx;
    struct queue *q;

    switch (qr->op) {
        case QUEUE_OP_ADD: {
            json_t *config;
            /* queue_parse_config() derives requires = ["+NAME"] from
             * 'hosts', matching a configured hosts queue.
             */
            if (!(config = json_pack ("{s:s}", "hosts", qr->hosts))) {
                errprintf (errp, "out of memory");
                errno = ENOMEM;
                return -1;
            }
            q = queues_add (qctx->queues, qr->name, config, errp);
            json_decref (config);
            if (!q)
                return -1;
            if (queue_set_runtime (q, true, qr->hosts) < 0) {
                errprintf (errp, "out of memory");
                (void)queues_remove (qctx->queues, qr->name, NULL);
                return -1;
            }
            queue_runtime_notify (qctx);
            return 0;
        }
        case QUEUE_OP_REMOVE:
            if (queues_remove (qctx->queues, qr->name, errp) < 0)
                return -1;
            queue_runtime_notify (qctx);
            return 0;
        case QUEUE_OP_START:
            return queues_start_queue (qctx->queues,
                                       qr->name,
                                       qr->nocheckpoint,
                                       errp);
        case QUEUE_OP_STOP:
            /* Already applied before the property update; see
             * queue_runtime_start().
             */
            return 0;
        case QUEUE_OP_ENABLE:
            return queues_enable_queue (qctx->queues, qr->name, errp);
        case QUEUE_OP_DISABLE:
            return queues_disable_queue (qctx->queues,
                                         qr->name,
                                         qr->reason,
                                         errp);
    }
    errprintf (errp, "unknown operation");
    errno = EINVAL;
    return -1;
}

/* The resource module applied the property change, so the queue table
 * can be updated to match.
 */
static void queue_runtime_continuation (flux_future_t *f, void *arg)
{
    struct queue_rpc *qr = arg;
    struct queue_ctx *qctx = qr->qctx;
    flux_t *h = qctx->ctx->h;
    flux_error_t error;
    const char *errmsg = NULL;

    if (flux_future_get (f, NULL) < 0) {
        errmsg = future_strerror (f, errno);
        goto error;
    }
    if (queue_runtime_apply (qr, &error) < 0) {
        errmsg = error.text;
        goto error;
    }
    if (flux_respond (h, qr->msg, NULL) < 0)
        flux_log_error (h, "error responding to %s", queue_op_topic (qr->op));
    goto done;
error:
    /* R and the key disagree now, so put the key back the way the queue
     * table still has it.
     */
    queue_runtime_revert (qctx);
    if (flux_respond_error (h, qr->msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to %s", queue_op_topic (qr->op));
done:
    flux_future_destroy (f);
    queue_rpc_destroy (qr);
}

/* Send the resource.property-update this operation needs, if any, and
 * hand 'qr' to the continuation.  Returns 0 if the operation is complete
 * or in flight, -1 with 'errmsgp' set if it failed outright.
 */
static int queue_runtime_property_update (struct queue_rpc *qr,
                                          const char **errmsgp)
{
    struct queue_ctx *qctx = qr->qctx;
    flux_t *h = qctx->ctx->h;
    char prop[QUEUE_PROPERTY_MAX];
    flux_future_t *f = NULL;

    (void)snprintf (prop, sizeof (prop), "+%s", qr->name);
    switch (qr->op) {
        case QUEUE_OP_ADD:
            f = flux_rpc_pack (h,
                               "resource.property-update",
                               0,
                               0,
                               "{s:{s:s}}",
                               "property-add", prop, qr->hosts);
            break;
        case QUEUE_OP_REMOVE:
            /* The property and the exclusive set entry go in one update,
             * so the ranks never linger as exclusive for a queue that is
             * gone.
             */
            f = flux_rpc_pack (h,
                               "resource.property-update",
                               0,
                               0,
                               "{s:{s:s} s:[s]}",
                               "property-remove", prop, qr->hosts,
                               "exclusive-remove", prop);
            break;
        case QUEUE_OP_START:
        case QUEUE_OP_STOP:
            f = flux_rpc_pack (h,
                               "resource.property-update",
                               0,
                               0,
                               "{s:[s]}",
                               qr->op == QUEUE_OP_START ? "exclusive-add"
                                                        : "exclusive-remove",
                               prop);
            break;
        case QUEUE_OP_ENABLE:
        case QUEUE_OP_DISABLE: {
            /* Neither changes R, so apply and respond here rather than
             * in a continuation.  The response is sent from this branch
             * so that 'error' does not have to outlive it.
             */
            flux_error_t error;
            if (queue_runtime_apply (qr, &error) < 0) {
                queue_runtime_revert (qctx);
                if (flux_respond_error (h, qr->msg, errno, error.text) < 0)
                    flux_log_error (h,
                                    "error responding to %s",
                                    queue_op_topic (qr->op));
            }
            else if (flux_respond (h, qr->msg, NULL) < 0)
                flux_log_error (h,
                                "error responding to %s",
                                queue_op_topic (qr->op));
            queue_rpc_destroy (qr);
            return 0;
        }
    }
    if (!f || flux_future_then (f, -1., queue_runtime_continuation, qr) < 0) {
        flux_future_destroy (f);
        queue_runtime_revert (qctx);
        *errmsgp = "error sending resource.property-update request";
        return -1;
    }
    return 0; // 'qr' is owned by the continuation now
}

/* Commit the proposed key and wait, then continue the chain.
 *
 * The commit is synchronous: it is the write-ahead step, and nothing
 * else may proceed until it lands.  The busy flag held by 'qr' keeps a
 * second operation from interleaving with this one.
 */
static int queue_runtime_begin (struct queue_rpc *qr,
                                json_t *entry,
                                const char *remove_name,
                                const char **errmsgp)
{
    flux_t *h = qr->qctx->ctx->h;
    json_t *o = NULL;
    flux_future_t *f = NULL;
    int rc = -1;

    if (!(o = runtime_queues_encode (qr->qctx->queues,
                                     entry ? qr->name : NULL,
                                     entry,
                                     remove_name))
        || !(f = runtime_queues_commit (h, o))
        || flux_future_get (f, NULL) < 0) {
        *errmsgp = "error committing " RUNTIME_QUEUES_KEY;
        goto done;
    }
    rc = queue_runtime_property_update (qr, errmsgp);
done:
    flux_future_destroy (f);
    json_decref (o);
    return rc;
}

/* Reject a second runtime queue operation while one is in flight.
 *
 * Each operation builds its proposed key from in-memory state, which
 * changes only after the property update succeeds.  Two overlapping
 * operations would therefore each build from a state that lacks the
 * other's change, and the second commit would drop the first.
 */
static bool queue_runtime_busy (struct queue_ctx *qctx, const char **errmsgp)
{
    if (qctx->busy) {
        *errmsgp = "runtime queue operation in progress";
        errno = EBUSY;
        return true;
    }
    return false;
}

/* Create a queue at runtime (RFC 33 runtime queues).
 *
 * The queue is created enabled and stopped, so that users may submit to
 * it before it starts.
 */
static void queue_add_cb (flux_t *h,
                          flux_msg_handler_t *mh,
                          const flux_msg_t *msg,
                          void *arg)
{
    struct queue_ctx *qctx = arg;
    const char *errmsg = NULL;
    const char *name;
    const char *hosts;
    struct queue_rpc *qr = NULL;
    json_t *entry = NULL;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:s s:s}",
                             "name", &name,
                             "hosts", &hosts) < 0)
        goto error;
    if (queue_runtime_busy (qctx, &errmsg))
        goto error;
    if (!queues_have_named (qctx->queues)) {
        errmsg = "runtime queues require named queues";
        errno = EINVAL;
        goto error;
    }
    if (queues_lookup (qctx->queues, name, NULL)) {
        errmsg = "queue already exists";
        errno = EEXIST;
        goto error;
    }
    /* The membership property is "+NAME", so the name must be a valid
     * RFC 20 property name, as for a configured 'hosts' queue.
     */
    if (strlen (name) == 0
        || strlen (name) > QUEUE_NAME_MAX
        || strpbrk (name, QUEUE_BANNED_PROPERTY_CHARS)) {
        errmsg = "queue name is not a valid property string (RFC 20)";
        errno = EINVAL;
        goto error;
    }
    if (strlen (hosts) == 0) {
        errmsg = "hosts must not be empty";
        errno = EINVAL;
        goto error;
    }
    if (!(entry = json_pack ("{s:s s:b s:b}",
                             "hosts", hosts,
                             "enabled", 1,
                             "started", 0))) {
        errno = ENOMEM;
        goto error;
    }
    if (!(qr = queue_rpc_create (qctx, msg, QUEUE_OP_ADD, name, hosts, NULL)))
        goto error;
    if (queue_runtime_begin (qr, entry, NULL, &errmsg) < 0) {
        queue_rpc_destroy (qr);
        goto error;
    }
    json_decref (entry);
    return;
error:
    json_decref (entry);
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to job-manager.queue-add");
}

/* Return true if any active job is in queue 'name' and has not yet been
 * allocated resources.
 */
static bool queue_has_pending_jobs (struct queue_ctx *qctx, const char *name)
{
    struct job *job = zhashx_first (qctx->ctx->active_jobs);

    while (job) {
        if (queue_name_covers (qctx, name, job->queue)
            && job->state != FLUX_JOB_STATE_INACTIVE
            && job->state <= FLUX_JOB_STATE_SCHED)
            return true;
        job = zhashx_next (qctx->ctx->active_jobs);
    }
    return false;
}

/* Remove a runtime queue and its property.
 */
static void queue_remove_cb (flux_t *h,
                             flux_msg_handler_t *mh,
                             const flux_msg_t *msg,
                             void *arg)
{
    struct queue_ctx *qctx = arg;
    flux_error_t error;
    const char *errmsg = NULL;
    const char *name;
    struct queue *q;
    struct queue_rpc *qr = NULL;

    if (flux_request_unpack (msg, NULL, "{s:s}", "name", &name) < 0)
        goto error;
    if (queue_runtime_busy (qctx, &errmsg))
        goto error;
    if (!(q = queues_lookup (qctx->queues, name, &error))) {
        errmsg = error.text;
        errno = ENOENT;
        goto error;
    }
    if (!queue_is_runtime (q)) {
        errmsg = "only a runtime queue may be removed";
        errno = EINVAL;
        goto error;
    }
    /* Raising exceptions on pending jobs is out of scope for now, so
     * refuse rather than strand them in a queue that no longer exists.
     */
    if (queue_has_pending_jobs (qctx, name)) {
        errmsg = "queue has pending jobs";
        errno = EBUSY;
        goto error;
    }
    if (queue_is_started (q)
        && queues_stop_queue (qctx->queues,
                              name,
                              "queue removed",
                              1,
                              &error) < 0) {
        errmsg = error.text;
        errno = EINVAL;
        goto error;
    }
    if (!(qr = queue_rpc_create (qctx,
                                 msg,
                                 QUEUE_OP_REMOVE,
                                 name,
                                 queue_hosts (q),
                                 NULL)))
        goto error;
    if (queue_runtime_begin (qr, NULL, name, &errmsg) < 0) {
        queue_rpc_destroy (qr);
        goto error;
    }
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to job-manager.queue-remove");
}

static void queue_enable_cb (flux_t *h,
                             flux_msg_handler_t *mh,
                             const flux_msg_t *msg,
                             void *arg)
{
    struct queue_ctx *qctx = arg;
    flux_error_t error;
    const char *errmsg = NULL;
    const char *name = NULL;
    int enable;
    const char *disable_reason = NULL;
    int all;
    struct queue *q;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s?s s:b s?s s:b}",
                             "name", &name,
                             "enable", &enable,
                             "reason", &disable_reason,
                             "all", &all) < 0)
        goto error;
    if (!enable && !disable_reason) {
        errmsg = "reason is required for disable";
        errno = EINVAL;
        goto error;
    }
    if (!name && queues_have_named (qctx->queues) && !all) {
        errmsg = "Use --all to apply this command to all queues";
        errno = EINVAL;
        goto error;
    }
    /* A runtime queue records the change in the KVS first, so that the
     * enabled bit survives a job-manager restart.  Neither enable nor
     * disable changes R, so there is no property update in the chain.
     */
    if (name && (q = queues_lookup (qctx->queues, name, NULL))
        && queue_is_runtime (q)) {
        struct queue_rpc *qr;
        json_t *entry;

        if (queue_runtime_busy (qctx, &errmsg))
            goto error;
        if (!(entry = runtime_queue_entry (q,
                                           -1,
                                           enable ? 1 : 0,
                                           enable ? NULL : disable_reason))) {
            errmsg = "out of memory";
            goto error;
        }
        if (!(qr = queue_rpc_create (qctx,
                                     msg,
                                     enable ? QUEUE_OP_ENABLE
                                            : QUEUE_OP_DISABLE,
                                     name,
                                     queue_hosts (q),
                                     disable_reason))) {
            json_decref (entry);
            goto error;
        }
        if (queue_runtime_begin (qr, entry, NULL, &errmsg) < 0) {
            queue_rpc_destroy (qr);
            json_decref (entry);
            goto error;
        }
        json_decref (entry);
        return;
    }
    if (enable) {
        if (queues_enable_queue (qctx->queues, name, &error) < 0) {
            errmsg = error.text;
            errno = EINVAL;
            goto error;
        }
    }
    else {
        if (queues_disable_queue (qctx->queues,
                                  name,
                                  disable_reason,
                                  &error) < 0) {
            errmsg = error.text;
            errno = EINVAL;
            goto error;
        }
    }
    if (flux_respond (h, msg, NULL) < 0)
        flux_log_error (h, "error responding to job-manager.queue-enable");
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to job-manager.queue-enable");
}

static void queue_start_cb (flux_t *h,
                            flux_msg_handler_t *mh,
                            const flux_msg_t *msg,
                            void *arg)
{
    struct queue_ctx *qctx = arg;
    flux_error_t error;
    const char *errmsg = NULL;
    const char *name = NULL;
    int start;
    const char *stop_reason = NULL;
    int all;
    int nocheckpoint = 0;
    struct queue *q;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s?s s:b s?s s:b s?b}",
                             "name", &name,
                             "start", &start,
                             "reason", &stop_reason,
                             "all", &all,
                             "nocheckpoint", &nocheckpoint) < 0)
        goto error;
    if (!name && queues_have_named (qctx->queues) && !all) {
        errmsg = "Use --all to apply this command to all queues";
        errno = EINVAL;
        goto error;
    }
    /* An --all start that spans an exclusive queue would need one update
     * per queue, each answered before the next starts.  Out of scope for
     * now: require that an exclusive queue be started by name.
     *
     * A stop is let through, because instance shutdown stops all queues
     * and must not fail.  The queues are stopped synchronously below and
     * the exclusive set is released afterwards without waiting, which
     * preserves the ordering that matters: no new job from an exclusive
     * queue starts after its nodes are returned.
     */
    if (!name && start && queues_have_exclusive (qctx->queues)) {
        errmsg = "--all is not supported with exclusive queues";
        errno = EINVAL;
        goto error;
    }
    /* An exclusive queue takes its nodes by updating the resource
     * module's exclusive set, and records the change in the KVS first.
     * A queue that is already in the requested state takes the
     * synchronous path below, which is a no-op for it.
     */
    if (name
        && (q = queues_lookup (qctx->queues, name, NULL))
        && queue_is_exclusive (q)
        && (start ? !queue_is_started (q) : queue_is_started (q))) {
        struct queue_rpc *qr;
        json_t *entry;

        if (queue_runtime_busy (qctx, &errmsg))
            goto error;
        /* A stop is applied before the property update, so that no new
         * job from the queue starts after the nodes are returned.  A
         * start is applied after, so that by the time the queue's jobs
         * are considered, other jobs are already excluded.
         */
        if (!start
            && queues_stop_queue (qctx->queues,
                                  name,
                                  stop_reason,
                                  nocheckpoint,
                                  &error) < 0) {
            errmsg = error.text;
            errno = EINVAL;
            goto error;
        }
        if (!(entry = runtime_queue_entry (q, start ? 1 : 0, -1, stop_reason))) {
            errmsg = "out of memory";
            goto error;
        }
        if (!(qr = queue_rpc_create (qctx,
                                     msg,
                                     start ? QUEUE_OP_START : QUEUE_OP_STOP,
                                     name,
                                     queue_hosts (q),
                                     stop_reason))) {
            json_decref (entry);
            goto error;
        }
        qr->nocheckpoint = nocheckpoint;
        if (queue_runtime_begin (qr, entry, NULL, &errmsg) < 0) {
            queue_rpc_destroy (qr);
            json_decref (entry);
            goto error;
        }
        json_decref (entry);
        return;
    }
    if (start) {
        if (queues_start_queue (qctx->queues,
                                name,
                                nocheckpoint,
                                &error) < 0) {
            errmsg = error.text;
            errno = EINVAL;
            goto error;
        }
    }
    else {
        if (queues_stop_queue (qctx->queues,
                               name,
                               stop_reason,
                               nocheckpoint,
                               &error) < 0) {
            errmsg = error.text;
            errno = EINVAL;
            goto error;
        }
        if (!name)
            queue_exclusive_release_all (qctx);
    }
    if (flux_respond (h, msg, NULL) < 0)
        flux_log_error (h, "error responding to job-manager.queue-start");
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "error responding to job-manager.queue-start");
}

static const struct flux_msg_handler_spec htab[] = {
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-list",
        queue_list_cb,
        FLUX_ROLE_USER
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-status",
        queue_status_cb,
        FLUX_ROLE_USER
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-enable",
        queue_enable_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-start",
        queue_start_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-add",
        queue_add_cb,
        0,
    },
    {
        FLUX_MSGTYPE_REQUEST,
        "job-manager.queue-remove",
        queue_remove_cb,
        0,
    },
    FLUX_MSGHANDLER_TABLE_END,
};

void queue_ctx_destroy (struct queue_ctx *qctx)
{
    if (qctx) {
        int saved_errno = errno;
        conf_unregister_callback (qctx->ctx->conf, queue_configure);
        flux_msg_handler_delvec (qctx->handlers);
        queues_destroy (qctx->queues);
        queues_destroy (qctx->saved);
        free (qctx);
        errno = saved_errno;
    }
}

/*  Test equality of two constraint objects.
 *  For now, two constraints are equivalent if:
 *
 *  - both are either NULL or empty objects (i.e. size == 0)
 *    (Note: json_object_size (NULL) == 0)
 *
 *  - json_equal(a, b) returns true
 */
static bool constraints_equal (json_t *c1, json_t *c2)
{
    if ((json_object_size (c1) == 0 && json_object_size (c2) == 0)
        || json_equal (c1, c2))
        return true;
    return false;
}

static int constraints_match_check (struct queue_ctx *qctx,
                                    const char *name,
                                    json_t *constraints,
                                    flux_error_t *errp)
{
    int rc = -1;
    json_t *expected = NULL;
    struct queue *q;

    /*  Return an error if the job's current queue doesn't exist since we
     *  can't validate current constraints (This should not happen in normal
     *  situations).
     */
    if (!(q = queues_lookup (qctx->queues, name, errp)))
        return -1;

    /*  If current queue has constraints, then create a constraint object
     *  for equivalence test below. queue_requires() returns the effective
     *  requires: for a virtual queue (RFC 33) its own requires is always
     *  NULL (enforced by conf_policy.c), but its jobs carry the parent's
     *  property constraint (injected by the constraints frobnicator
     *  plugin), so the parent's requires is what must match here.
     */
    if (queue_requires (q)
        && !(expected = json_pack ("{s:O}",
                                   "properties",
                                   queue_requires (q)))) {
        errprintf (errp, "failed to get constraints for current queue");
        goto out;
    }

    /*  Constraints of current job and queue must match exactly or queue
     *  update will be rejected. This is because the entire constraints
     *  object will be overwritten on queue update, and we do not want to
     *  replace any extra constraints provided on the submission commandline
     *  (and these likely wouldn't make sense in the new queue anyway)
     */
    if (!constraints_equal (constraints, expected)) {
        errprintf (errp,
                   "job appears to have non-queue constraints, "
                   "unable to update queue to %s",
                   name);
        goto out;
    }
    rc = 0;
out:
    json_decref (expected);
    return rc;
}

static int queue_update_cb (flux_plugin_t *p,
                            const char *topic,
                            flux_plugin_arg_t *args,
                            void *arg)
{
    int rc;
    struct queue_ctx *qctx = arg;
    flux_job_state_t state;
    const char *name;
    const char *current_queue = NULL;
    json_t *constraints = NULL;
    flux_error_t error;
    struct queue *newq;

    if (flux_plugin_arg_unpack (args,
                                FLUX_PLUGIN_ARG_IN,
                                "{s:s s:i s:{s:{s:{s?s s?o}}}}",
                                "value", &name,
                                "state", &state,
                                "jobspec",
                                 "attributes",
                                  "system",
                                   "queue", &current_queue,
                                   "constraints", &constraints) < 0) {
        flux_jobtap_error (p, args, "plugin args unpack failed");
        return -1;
    }
    if (state == FLUX_JOB_STATE_RUN
        || state == FLUX_JOB_STATE_CLEANUP) {
        flux_jobtap_error (p,
                           args,
                           "update of queue for running job not supported");
        return -1;
    }
    if (current_queue && streq (current_queue, name)) {
        flux_jobtap_error (p,
                           args,
                           "job queue is already set to %s",
                           name);
        return -1;
    }
    if (!(newq = queues_lookup (qctx->queues, name, &error))) {
        flux_jobtap_error (p, args, "%s", error.text);
        return -1;
    }
    if (!queue_is_enabled (newq)) {
        flux_jobtap_error (p,
                           args,
                           "queue %s is currently disabled",
                           name);
        return -1;
    }
    /*  Constraints must match current queue exactly since they will be
     *  overwritten with new queue constraints after queue is updated:
     */
    if (constraints_match_check (qctx, current_queue, constraints, &error)) {
        flux_jobtap_error (p, args, "%s", error.text);
        return -1;
    }
    /*  Request the update service do a feasibility check for this update
     *  and append an additional update of the job constraints.
     *
     *  This is done via two different calls below dependent on whether the
     *  new queue has any constraints. As above, queue_requires() is the
     *  effective requires: a job moved into a virtual queue must pick up
     *  the parent's constraint, since that is what makes it schedule as
     *  part of the parent's job list.
     */
    if (queue_requires (newq)) {
        /*  Replace current constraints with those of the new queue
         */
        rc = flux_plugin_arg_pack (args,
                                   FLUX_PLUGIN_ARG_OUT,
                                   "{s:i s:{s:{s:O}}}",
                                   "feasibility", 1,
                                   "updates",
                                    "attributes.system.constraints",
                                     "properties",
                                     queue_requires (newq));
    }
    else {
        /*  New queue has no requirements. Set constraints to empty object.
         */
        rc = flux_plugin_arg_pack (args,
                                   FLUX_PLUGIN_ARG_OUT,
                                   "{s:i s:{s:{}}}",
                                   "feasibility", 1,
                                   "updates",
                                    "attributes.system.constraints");
    }
    /*  If either of the above packs failed then return an error:
     */
    if (rc < 0) {
        flux_jobtap_error (p,
                           args,
                           "unable to create jobtap out arguments");
        return -1;
    }
    return 0;
}

static int update_queue_plugin_init (flux_plugin_t *p, void *arg)
{
    return flux_plugin_add_handler (p,
                                    "job.update.attributes.system.queue",
                                    queue_update_cb,
                                    arg);
}

json_t *queue_ctx_save (struct queue_ctx *qctx)
{
    return queues_save (qctx->queues);
}

/* Apply post-restore side effects for one queue: if stop_on_restart
 * is set, override a restored started state to stopped with an
 * automated reason (fires notify, which dequeues); otherwise cancel
 * alloc requests for a queue restored in a stopped state.
 */
static int restore_side_effects (struct queue_ctx *qctx, struct queue *q)
{
    if (queue_is_started (q) && qctx->stop_on_restart) {
        if (queue_stop (q,
                        "Automatically stopped due to restart",
                        false) < 0)
            return -1;
    }
    else if (!queue_is_started (q))
        dequeue_jobs (qctx, queue_name (q));
    return 0;
}

static int restore_named_queues (struct queue_ctx *qctx)
{
    zlistx_t *names;
    const char *name;
    struct queue *q;

    if (!(names = queues_list_names (qctx->queues)))
        return -1;
    name = zlistx_first (names);
    while (name) {
        if ((q = queues_lookup (qctx->queues, name, NULL))
            && restore_side_effects (qctx, q) < 0) {
            zlistx_destroy (&names);
            return -1;
        }
        name = zlistx_next (names);
    }
    zlistx_destroy (&names);
    return 0;
}

/* Recreate the runtime queues recorded in the queues.runtime key.
 *
 * This runs before jobs are loaded, so that a job restored into a
 * runtime queue finds its queue already present.
 *
 * No property update is sent here, and none is needed: the resource
 * module reads the same key when it defines R, so the properties and
 * the exclusive set are already in place.  A job-manager restart alone
 * therefore does not touch R.
 *
 * N.B. the key is not rewritten here.  An entry skipped below stays in
 * the key until the next save, which encodes only the queues that are
 * in memory and so drops it then.
 */
int queue_ctx_restore_runtime (struct queue_ctx *qctx)
{
    flux_t *h = qctx->ctx->h;
    json_t *queues = NULL;
    const char *name;
    json_t *entry;
    int rc = -1;

    if (runtime_queues_lookup (h, &queues) < 0)
        return -1;
    if (!queues)
        return 0;
    if (json_object_size (queues) > 0 && !queues_have_named (qctx->queues)) {
        flux_log (h,
                  LOG_ERR,
                  "%s: runtime queues require named queues, ignoring all",
                  RUNTIME_QUEUES_KEY);
        rc = 0;
        goto done;
    }
    json_object_foreach (queues, name, entry) {
        const char *hosts;
        const char *disable_reason = NULL;
        const char *stop_reason = NULL;
        int enabled = 1;
        int started = 0;
        json_t *config;
        struct queue *q;
        flux_error_t error;

        if (json_unpack (entry,
                         "{s:s s?b s?b s?s s?s}",
                         "hosts", &hosts,
                         "enabled", &enabled,
                         "started", &started,
                         "disable_reason", &disable_reason,
                         "stop_reason", &stop_reason) < 0) {
            flux_log (h,
                      LOG_ERR,
                      "%s: queue %s: malformed entry, ignored",
                      RUNTIME_QUEUES_KEY,
                      name);
            continue;
        }
        /* The configuration is the authority: a configured queue of the
         * same name wins, and the runtime one is dropped. The resource
         * module makes the same choice for the property.
         */
        if (queues_lookup (qctx->queues, name, NULL)) {
            flux_log (h,
                      LOG_ERR,
                      "runtime queue %s conflicts with configured queue;"
                      " ignored",
                      name);
            continue;
        }
        if (!(config = json_pack ("{s:s}", "hosts", hosts))) {
            errno = ENOMEM;
            goto done;
        }
        q = queues_add (qctx->queues, name, config, &error);
        json_decref (config);
        if (!q) {
            flux_log (h,
                      LOG_ERR,
                      "runtime queue %s: %s; ignored",
                      name,
                      error.text);
            continue;
        }
        if (queue_set_runtime (q, true, hosts) < 0)
            goto done;
        if (!enabled && queue_disable (q, disable_reason) < 0)
            goto done;
        /* nocheckpoint: the key is this queue's record, not the
         * job-manager checkpoint.
         */
        if (started) {
            if (queues_start_queue (qctx->queues, name, 1, &error) < 0) {
                flux_log (h, LOG_ERR, "runtime queue %s: %s", name, error.text);
                goto done;
            }
        }
        else if (queues_stop_queue (qctx->queues,
                                    name,
                                    stop_reason,
                                    1,
                                    &error) < 0) {
            flux_log (h, LOG_ERR, "runtime queue %s: %s", name, error.text);
            goto done;
        }
        flux_log (h,
                  LOG_INFO,
                  "restored runtime queue %s (hosts=%s, %s, %s)",
                  name,
                  hosts,
                  enabled ? "enabled" : "disabled",
                  started ? "started" : "stopped");
    }
    rc = 0;
done:
    json_decref (queues);
    return rc;
}

int queue_ctx_restore (struct queue_ctx *qctx, int version, json_t *o)
{
    /* Apply saved state via queues_restore, which does not fire change
     * notifications, then apply side effects for the resulting state
     * here.
     */
    if (queues_restore (qctx->queues, version, o) < 0)
        return -1;

    if (version == 1) {
        if (!queues_have_named (qctx->queues)) {
            struct queue *q = queues_lookup (qctx->queues, NULL, NULL);
            if (q && restore_side_effects (qctx, q) < 0)
                return -1;
        }
        else if (restore_named_queues (qctx) < 0)
            return -1;
    }
    return 0;
}

int queue_submit_check (struct queue_ctx *qctx,
                        json_t *jobspec,
                        flux_error_t *error)
{
    struct queue *q;
    json_t *o;
    const char *name = NULL;

    if ((o = jpath_get (jobspec, "attributes.system.queue")))
        name = json_string_value (o);

    if (!(q = queues_lookup (qctx->queues, name, error))) {
        errno = EINVAL;
        return -1;
    }
    if (!queue_is_enabled (q)) {
        errprintf (error, "job submission%s%s is disabled: %s",
                   name ? " to " : "",
                   name ? name : "",
                   queue_disable_reason (q));
        errno = EINVAL;
        return -1;
    }
    return 0;
}

bool queue_started (struct queue_ctx *qctx, struct job *job)
{
    return queues_queue_is_started (qctx->queues, job->queue);
}

struct queue_ctx *queue_ctx_create (struct job_manager *ctx)
{
    struct queue_ctx *qctx;
    flux_error_t error;

    if (!(qctx = calloc (1, sizeof (*qctx))))
        return NULL;
    qctx->ctx = ctx;
    qctx->stop_on_restart = false;

    if (!(qctx->queues = queues_create ()))
        goto error;
    queues_set_notify (qctx->queues, on_queue_change, qctx);

    if (flux_msg_handler_addvec (ctx->h,
                                 htab,
                                 qctx,
                                 &qctx->handlers) < 0)
        goto error;
    if (conf_register_callback (ctx->conf,
                                &error,
                                "queue",
                                queue_configure,
                                queue_rollback,
                                qctx) < 0) {
        flux_log (ctx->h,
                  LOG_ERR,
                  "error parsing queue config: %s",
                  error.text);
        goto error;
    }
    if (jobtap_register_builtin (ctx->jobtap,
                                 ".update-queue",
                                 update_queue_plugin_init,
                                 qctx) < 0
        || !jobtap_load (ctx->jobtap, ".update-queue", NULL, NULL)) {
        flux_log (ctx->h,
                  LOG_ERR,
                  "Failed to register and load update-queue plugin");
        goto error;
    }
    return qctx;
error:
    queue_ctx_destroy (qctx);
    return NULL;
}

// vi:ts=4 sw=4 expandtab
