/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/*  exec: distribute job execution
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <flux/core.h>
#include <flux/idset.h>
#include <sys/wait.h>
#define EXIT_CODE(x) __W_EXITCODE(x,0)

#include "ccan/str/str.h"

#include "derp.h"
#include "job.h"
#include "hello.h"
#include "exec/barrier.h"

extern char **environ;

struct exec {
    struct derp_ctx *ctx;
    zhashx_t *jobs;

    struct idset *idset;        // Idset of this rank plus all downstream peers
    struct hello_responder *hr;

    flux_watcher_t *hr_timer;

    flux_msg_handler_t **handlers;
};

static const char *job_shell_path (struct derp_job *job)
{
    return flux_conf_builtin_get ("shell_path", FLUX_CONF_AUTO);
}

static void exec_verror (struct derp_job *job, const char *fmt, va_list ap)
{
    int n;
    char note [256];
    int len = sizeof (note);
    flux_t *h = job->exec->ctx->h;
    flux_future_t *f;

    if ((n = vsnprintf (note, len, fmt, ap)) < 0)
        strcpy (note, "(msg format failed)");
    else if (n >= len) {
        note [len-2] = '+';
        note [len-1] = '\0';
    }
    if (!(f = flux_rpc_pack (h,
                            "derp.notify",
                            0,
                            0,
                            "{s:s s:{s:I s:i s:s s:s}}",
                            "type", "exception",
                            "data",
                              "id", job->id,
                              "severity", 0,
                              "type", "exec",
                              "note", note)))
        flux_log_error (h,
                        "%ju: failed to send exception: %s",
                        (uintmax_t) job->id,
                        note);
    flux_future_destroy (f);
}

static void exec_error (struct derp_job *job, const char *fmt, ...)
{
    va_list ap;
    int saved_errno = errno;
    va_start (ap, fmt);
    exec_verror (job, fmt, ap);
    va_end (ap);
    errno = saved_errno;
}


/*  Complete: */

static int exec_notify_finish (struct derp_job *job)
{
    flux_t *h = job->exec->ctx->h;

    if (idset_equal (job->finish_ranks, job->subtree_ranks)) {
        if (job->request) {
            flux_log (h,
                      LOG_DEBUG,
                      "%ju: notify: finish status=%d",
                      (uintmax_t) job->id,
                      job->status);
            if (flux_respond_pack (h,
                                   job->request,
                                   "{s:I s:s s:{s:i}}",
                                   "id", job->id,
                                   "type", "finish",
                                   "data",
                                   "status", job->status) < 0) {
                exec_error (job,
                            "finish notification failed: %s",
                            strerror (errno));
                return -1;
            }
        }
        else {
            flux_future_t *f;
            char *ranks;

            if (!(ranks = idset_encode (job->finish_ranks, IDSET_FLAG_RANGE)))
                return -1;

            flux_log (h,
                      LOG_DEBUG,
                      "%ju: notifying upstream: finish on ranks %s",
                      (uintmax_t) job->id,
                      ranks);
            f = flux_rpc_pack (h,
                              "derp.notify",
                               FLUX_NODEID_UPSTREAM,
                               FLUX_RPC_NORESPONSE,
                              "{s:s s:{s:I s:s s:i}}",
                              "type", "finish",
                              "data",
                                "id", job->id,
                                "ranks", ranks,
                                "status", job->status);
            free (ranks);
            if (!f) {
                exec_error (job,
                            "finish notification failed: %s",
                            strerror (errno));
                return -1;
            }
            flux_future_destroy (f);
        }
    }
    return 0;
}

static int exec_job_finish (struct exec *exec,
                            flux_jobid_t id,
                            const char *ranks,
                            int status)
{
    int rc = -1;
    struct derp_job *job;
    struct idset *ids = NULL;

    if (!(ids = idset_decode (ranks)))
        goto error;
    if (!(job = zhashx_lookup (exec->jobs, &id))) {
        errno = ENOENT;
        goto error;
    }
    if (idset_add (job->finish_ranks, ids) < 0)
        goto error;
    if (status > job->status)
        job->status = status;
    if (exec_notify_finish (job) < 0)
        goto error;
    rc = 0;
error:
    idset_destroy (ids);
    return rc;
}

static void exec_complete_cb (flux_subprocess_t *p)
{
    struct derp_job *job = flux_subprocess_aux_get (p, "job");
    int status = flux_subprocess_status (p);

    if (status > job->status)
        job->status = status;

    if (idset_set (job->finish_ranks, job->exec->ctx->rank) < 0)
        flux_log_error (job->exec->ctx->h, "complete_cb: idset_set");

    if (exec_notify_finish (job) < 0)
        flux_log_error (job->exec->ctx->h, "exec_notify_finish");
}


/* Start: */

static int exec_notify_start (struct derp_job *job)
{
    flux_t *h = job->exec->ctx->h;
    char *s, *r;

    s = idset_encode (job->start_ranks, IDSET_FLAG_RANGE);
    r = idset_encode (job->subtree_ranks, IDSET_FLAG_RANGE);

    flux_log (h,
              LOG_DEBUG,
              "%ju: started on ranks %s of %s",
              (uintmax_t) job->id,
              s,
              r);
    free (s);
    free (r);

    if (idset_equal (job->start_ranks, job->subtree_ranks)) {
        flux_log (h,
                  LOG_DEBUG,
                  "%ju: subtree ranks started. notifying",
                  (uintmax_t) job->id);
        if (job->request) {
            flux_log (h,
                      LOG_DEBUG,
                      "%ju: notify: start",
                      (uintmax_t) job->id);
            if (flux_respond_pack (h,
                                   job->request,
                                   "{s:I s:s s:{}}",
                                   "id", job->id,
                                   "type", "start",
                                   "data") < 0) {
                flux_log_error (h, "exec_notify_start: flux_respond_pack");
                return -1;
            }
        }
        else {
            flux_future_t *f;
            char *ranks;

            if (!(ranks = idset_encode (job->start_ranks, IDSET_FLAG_RANGE)))
                return -1;

            flux_log (h,
                      LOG_DEBUG,
                      "%ju: notifying upstream: start on ranks %s",
                      (uintmax_t) job->id,
                      ranks);
            f = flux_rpc_pack (h,
                               "derp.notify",
                               FLUX_NODEID_UPSTREAM,
                               FLUX_RPC_NORESPONSE,
                               "{s:s s:{s:I s:s}}",
                               "type", "start",
                               "data",
                               "id", job->id,
                               "ranks", ranks);
            free (ranks);
            if (!f) {
                flux_log_error (h, "exec_start_notify: flux_rpc_pack");
                return -1;
            }
            flux_future_destroy (f);
        }
    }
    return 0;
}

static int exec_job_started (struct exec *exec,
                             flux_jobid_t id,
                             const char *ranks)
{
    struct derp_job *job;
    struct idset *ids;
    int rc = -1;

    if (!(ids = idset_decode (ranks)))
        return -1;
    if (!(job = zhashx_lookup (exec->jobs, &id))) {
        errno = ENOENT;
        goto error;
    }
    if (idset_add (job->start_ranks, ids) < 0)
        goto error;
    rc = exec_notify_start (job);
error:
    idset_destroy (ids);
    return rc;
}


/*  Subprocess state update handler */

static void exec_state_cb (flux_subprocess_t *p, flux_subprocess_state_t state)
{
    struct derp_job *job = flux_subprocess_aux_get (p, "job");
    flux_t *h = job->exec->ctx->h;
    int rank = job->exec->ctx->rank;

    if (state == FLUX_SUBPROCESS_RUNNING) {
        flux_log (h, LOG_DEBUG, "%ju: running", (uintmax_t) job->id);
        if (idset_set (job->start_ranks, rank) < 0)
            flux_log_error (h, "exec_state_cb: idset_set");
        if (exec_notify_start (job) < 0)
            flux_log_error (h, "exec_notify_start");
    }
    else if (state == FLUX_SUBPROCESS_FAILED
            || state == FLUX_SUBPROCESS_EXEC_FAILED) {
        int errnum = flux_subprocess_fail_errno (p);
        int code = EXIT_CODE(1);

        if (errnum == EPERM || errnum == EACCES)
            code = EXIT_CODE(126);
        else if (errnum == ENOENT)
            code = EXIT_CODE(127);
        else if (errnum == EHOSTUNREACH)
            code = EXIT_CODE(68);

        if (code > job->status)
            job->status = code;

        idset_set (job->finish_ranks, job->exec->ctx->rank);

        if (exec_notify_finish (job) < 0)
            flux_log_error (h, "exec_state_cb: exec_notify");
    }
}


/*  Barrier:
 */
static void exec_barrier_complete (flux_future_t *f, void *arg)
{
    struct derp_job *job = arg;
    flux_t *h = job->exec->ctx->h;

    if (f && flux_future_get (f, NULL) < 0) {
        flux_log (job->exec->ctx->h, LOG_ERR, "barrier failed");
        exec_error (job, "barrier failure: %s", flux_future_error_string (f));
        return;
    }
    flux_log (h,
              LOG_DEBUG,
              "%ju: barrier %d complete",
              (uintmax_t) job->id,
              job->barrier->sequence);

    if (barrier_respond_all (h, job->barrier) < 0) {
        flux_log_error (h, "barrier_respond_all");
        // XXX: raise job exception
    }

    /*  If there is also a job process running on this rank
     *   release the barrier
     */
    if (job->p) {
        if (flux_subprocess_write (job->p,
                                   "FLUX_EXEC_PROTOCOL_FD",
                                   "exit=0\n",
                                   7) < 0)
            flux_log_error (h, "flux_subprocess_write: p=%p", job->p);
    }
    flux_future_destroy (f);
    barrier_reset (job->barrier);
}

static int exec_barrier_check (struct derp_job *job)
{
    flux_t *h = job->exec->ctx->h;
    flux_future_t *f = NULL;
    char *s;

    s = idset_encode (job->barrier->ranks, IDSET_FLAG_RANGE);
    flux_log (h,
              LOG_DEBUG,
              "%ju: exec_barrier_check: complete on %s",
              (uintmax_t) job->id,
              s);
    free (s);

    if (idset_equal (job->barrier->ranks, job->subtree_ranks)) {
        /*  Barrier is complete locally, if this is the lowest common
         *   ancestor for the whole job, then notify all downstream
         *   members via exec_barrier_complete(). O/w, just notify
         *   downstream.
         */
        if (idset_equal (job->ranks, job->subtree_ranks)) {
            flux_log (h,
                      LOG_DEBUG, "%ju: barrier %d complete on LCA\n",
                      (uintmax_t) job->id,
                      job->barrier->sequence);
            exec_barrier_complete (NULL, job);
            return 0;
        }
        flux_log (h,
                  LOG_DEBUG, "%ju: barrier notify upstream seq=%d\n",
                  (uintmax_t) job->id,
                  job->barrier->sequence);
        if (!(f = barrier_notify (h, job->id, job->barrier))
            || flux_future_then (f, -1., exec_barrier_complete, job) < 0)
            goto error;
    }
    return 0;
error:
    flux_log (h, LOG_ERR, "Barrier notify failure");
    return -1;
}

/*  Output: */

static void exec_output_cb (flux_subprocess_t *p, const char *stream)
{
    struct derp_job *job = flux_subprocess_aux_get (p, "job");
    flux_t *h = job->exec->ctx->h;
    const char *s;
    int len;

    s = flux_subprocess_getline (p, stream, &len);
    if (len == 0 || !s)
        return;

    if (streq (stream, "FLUX_EXEC_PROTOCOL_FD")) {
        if (!streq (s, "enter\n"))
            flux_log_error (h,
                            "%ju: local shell entered barrier with garbage: %s",
                             (uintmax_t) job->id,
                             s);
        if (barrier_enter_local (job->barrier, job->exec->ctx->rank) < 0)
            flux_log_error (h,
                            "%ju: barrier_enter",
                             (uintmax_t) job->id);
        flux_log (h,
                  LOG_DEBUG,
                  "%ju: local shell entered barrier %d",
                  (uintmax_t) job->id,
                  job->barrier->sequence);
        exec_barrier_check (job);
    }
    else if (s && len) {
        /* XXX: send output upstream or write to eventlog locally */
        flux_log (job->exec->ctx->h, LOG_INFO, "%s: %s", stream, s);
    }
}


/*  Subprocess: */

static flux_subprocess_ops_t ops = {
    .on_completion = exec_complete_cb,
    .on_state_change = exec_state_cb,
    .on_stdout = exec_output_cb,
    .on_stderr = exec_output_cb,
    .on_channel_out = exec_output_cb,
};

static int job_start (struct derp_job *job)
{
    struct exec *exec = job->exec;
    flux_t *h = exec->ctx->h;
    flux_cmd_t *cmd = NULL;
    char ns [128];

    if (!idset_test (job->ranks, exec->ctx->rank))
        return 0;

    if (!(cmd = flux_cmd_create (0, NULL, environ))) {
        flux_log_error (h, "flux_cmd_create");
        goto error;
    }
    if (flux_job_kvs_namespace (ns, sizeof (ns), job->id) < 0) {
        flux_log_error (h, "flux_job_kvs_namespace");
        goto error;
    }
    flux_cmd_setenvf (cmd, 1, "FLUX_KVS_NAMESPACE", "%s", ns);
    flux_cmd_argv_append (cmd, job_shell_path (job));
    flux_cmd_argv_appendf (cmd, "%ju", (uintmax_t) job->id);
    flux_cmd_setcwd (cmd, "/tmp");

    if (idset_count (job->ranks) > 1) {
        /* Setup barrier channel */
        if (flux_cmd_add_channel (cmd, "FLUX_EXEC_PROTOCOL_FD") < 0
            || flux_cmd_setopt (cmd,
                                "FLUX_EXEC_PROTOCOL_FD_LINE_BUFFER",
                                "true") < 0) {
            flux_log_error (h, "exec_init: flux_cmd_add_channel");
            goto error;
        }
    }

    int flags = 0;
    if (!(job->p = flux_rexec (h, exec->ctx->rank, flags, cmd, &ops))) {
        flux_log_error (h, "flux_rexec");
        goto error;
    }
    if (flux_subprocess_aux_set (job->p, "job", job, NULL) < 0) {
        flux_log_error (h, "flux_subprocess_aux_set");
        goto error;
    }
    flux_log (h,
              LOG_DEBUG,
              "%ju: started %s",
              (uintmax_t) job->id,
              flux_cmd_arg (cmd, 0));
    flux_cmd_destroy (cmd);
    return 0;
error:
    flux_cmd_destroy (cmd);
    return -1;
}

static struct derp_job * exec_job_add (struct exec *exec,
                                       flux_jobid_t id,
                                       uint32_t userid,
                                       const char *ranks)
{
    flux_t *h = exec->ctx->h;
    struct derp_job *job;

    /*  Do nothing if job already exists */
    if ((job = zhashx_lookup (exec->jobs, &id))) {
        flux_log (h,
                  LOG_DEBUG,
                  "%ju: exec_job_add duplicate request",
                  (uintmax_t) id);
        errno = EEXIST;
        return NULL;
    }

    if (!(job = derp_job_create (id, userid, ranks)))
        return NULL;

    job->exec = exec;
    job->subtree_ranks = idset_intersect (job->ranks, exec->idset);
    if (!job->subtree_ranks
        || zhashx_insert (exec->jobs, &job->id, job) < 0)
        goto error;
    if (idset_test (job->ranks, exec->ctx->rank)) {
        flux_log (h, LOG_DEBUG, "%ju: starting job shell", (uintmax_t) id);
        if (job_start (job) < 0)
            goto error;
    }
    return job;
error:
    derp_job_destroy (job);
    return NULL;
}


/*  Kill hello response handler */
static int derp_exec_kill (void *arg,
                           const char *name,
                           const char *idset,
                           json_t *data)
{
    struct exec *exec = arg;
    flux_t *h = exec->ctx->h;
    struct derp_job *job;
    flux_jobid_t id;
    int sig;
    char *s;

    s = json_dumps (data, 0);
    flux_log (h, LOG_DEBUG, "kill: %s", s);
    free (s);

    if (json_unpack (data, "{s:I s:i}", "id", &id, "signal", &sig) < 0) {
        flux_log (h, LOG_ERR, "kill: json_unpack failed");
        return -1;
    }

    if (!(job = zhashx_lookup (exec->jobs, &id))) {
        flux_log (h,
                  LOG_ERR,
                  "kill: %ju: job not found",
                  (uintmax_t) id);
        errno = ENOENT;
        return -1;
    }

    if (job->p) {
        flux_future_t *f = flux_subprocess_kill (job->p, sig);
        flux_future_destroy (f);
    }
    return 0;
}

/*  Handle 'kill' request from external source
 */
static void exec_kill (flux_t *h,
                       flux_msg_handler_t *mh,
                       const flux_msg_t *msg,
                       void *arg)
{
    struct exec *exec = arg;
    flux_jobid_t id;
    const char *ranks;
    int sig;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:I s:i s:s}",
                             "id", &id,
                             "signal", &sig,
                             "ranks", &ranks) < 0) {
        flux_log_error (h, "exec_kill: flux_request_unpack");
        return;
    }

    flux_log (h,
              LOG_DEBUG,
              "%ju: targets=%s: kill request received",
              (uintmax_t) id,
              ranks);

    if (derp_forward (exec->ctx,
                      "kill",
                      ranks,
                      "{s:I s:i}",
                      "id", id,
                      "signal", sig) < 0) {
        flux_log_error (h, "exec_kill: derp_forward");
        return;
    }
}

static int exec_state_update (void *arg,
                              const char *name,
                              const char *idset,
                              json_t *data)
{
    struct exec *exec = arg;
    flux_t *h = exec->ctx->h;
    json_t *jobs;
    size_t index;
    json_t *entry;
    char *s;

    s = json_dumps (data, 0);
    flux_log (h, LOG_DEBUG, "state-update: %s", s);
    free (s);

    if (json_unpack (data, "{s:o}", "jobs", &jobs) < 0
        || !json_is_array (jobs)) {
        errno = EPROTO;
        return -1;
    }
    json_array_foreach (jobs, index, entry) {
        flux_jobid_t id;
        uint32_t userid;
        const char *ranks;
        const char *type;
        if (json_unpack (entry,
                         "{s:I s:i s:s s:s}",
                         "id", &id,
                         "userid", &userid,
                         "type", &type,
                         "ranks", &ranks) < 0) {
            flux_log (exec->ctx->h,
                      LOG_ERR,
                      "Invalid job entry in state-update!");
            return -1;
        }
        if (streq (type, "add")) {
            struct derp_job *job;
            if (!(job = exec_job_add (exec, id, userid, ranks)))
                return -1;
        }
    }
    return 0;
}


/*  Handle job-exec "start" request from external source
 */
static void exec_start (flux_t *h,
                        flux_msg_handler_t *mh,
                        const flux_msg_t *msg,
                        void *arg)
{
    flux_jobid_t id;
    const char *ranks;
    uint32_t userid;
    struct idset *idset = NULL;
    struct exec *exec = arg;
    struct derp_job *job;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:I s:i s:s}",
                             "id", &id,
                             "userid", &userid,
                             "ranks", &ranks) < 0)
        goto error;

    flux_log (h,
              LOG_DEBUG,
              "%ju: targets=%s: exec_start request received",
              (uintmax_t) id,
              ranks);

    if (!(idset = idset_decode (ranks))) {
        flux_log_error (h, "idset_decode");
        goto error;
    }

    /* Accumulate hello response */
    if (idset_count (idset) > 1 || idset_first (idset) != exec->ctx->rank) {
        flux_log (h, LOG_DEBUG, "%ju: push add to peers", (uintmax_t) id);
        if (hello_responder_push (exec->hr, "add", id, userid, idset) < 0) {
            flux_log_error (h, "hello_responder_push");
            goto error;
        }
        if (hello_responder_count (exec->hr) == 1) {
            flux_timer_watcher_reset (exec->hr_timer, 0.02, 0.);
            flux_watcher_start (exec->hr_timer);
        }
    }

    /* Register job */
    if (!(job = exec_job_add (exec, id, userid, ranks)))
        goto error;

    job->request = flux_msg_incref (msg);

    idset_destroy (idset);
    return;
error:
    idset_destroy (idset);
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "exec_start: flux_respond_error");
}

/*  Handle 'start' request from downstream
 *
 *  Payload:
 *   { "id":I "ranks":s }
 */
static void exec_started (flux_t *h,
                          const flux_msg_t *msg,
                          json_t *data,
                          void *arg)
{
    struct exec *exec = arg;
    const char *ranks;
    flux_jobid_t id;

    if (json_unpack (data, "{s:I s:s}", "id", &id, "ranks", &ranks) < 0)
        goto error;
    if (exec_job_started (exec, id, ranks) < 0)
        goto error;
    return;
error:
    flux_log_error (h, "exec_started");
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "exec_started: flux_respond_error");
}

/*  Handle 'barrier' request from downstream peer(s)
 *
 *  Data:
 *   { "id":I "seq":i "ranks":s }
 */
static void exec_barrier (flux_t *h,
                          const flux_msg_t *msg,
                          json_t *data,
                          void *arg)
{
    struct exec *exec = arg;
    const char *ranks;
    flux_jobid_t id;
    struct derp_job *job = NULL;

    if (json_unpack (data, "{s:I s:s}", "id", &id, "ranks", &ranks) < 0) {
        flux_log_error (h, "flux_request_unpack: %s",
                        flux_msg_last_error (msg));
        goto error;
    }
    if (!(job = zhashx_lookup (exec->jobs, &id))) {
        errno = ENOENT;
        goto error;
    }
    flux_log (h,
              LOG_DEBUG,
              "%ju: %s entered barrier %d",
              (uintmax_t) job->id,
              ranks,
              job->barrier->sequence);

    if (barrier_enter (job->barrier, msg) < 0
        || exec_barrier_check (job) < 0)
        goto error;
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "exec_barrier: flux_respond_error");
}


/*  Handle 'finish' request from downstream peer(s)
 *
 *  Payload:
 *   { "id":I "ranks":s "status":i }
 */
static void exec_finish (flux_t *h,
                         const flux_msg_t *msg,
                         json_t *data,
                         void *arg)
{
    struct exec *exec = arg;
    const char *ranks;
    flux_jobid_t id;
    int status;

    if (json_unpack (data,
                     "{s:I s:s s:i}",
                     "id", &id,
                     "ranks", &ranks,
                     "status", &status) < 0) {
        flux_log (h, LOG_ERR, "exec_finish: %s", flux_msg_last_error (msg));
        goto error;
    }
    flux_log (h, LOG_DEBUG, "%ju: finish from %s", (uintmax_t) id, ranks);
    if (exec_job_finish (exec, id, ranks, status) < 0)
        goto error;
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "exec_finish: flux_respond_error");
}

/*  Handle 'release' request from downstream peer(s)
 *
 *  Payload:
 *   { "id":I "ranks":s }
 */
static void exec_release (flux_t *h,
                          const flux_msg_t *msg,
                          json_t *data,
                          void *arg)
{
    flux_respond_error (h, msg, ENOTSUP, NULL);
}


/*  Handle exception request from downstream peer(s)
 *
 *  Payload:
 *   { "id":I "severity":i "type":s "note":s }
 */
static void exec_exception (flux_t *h,
                            const flux_msg_t *msg,
                            json_t *data,
                            void *arg)
{
    struct exec *exec = arg;
    struct derp_job *job;
    flux_jobid_t id;
    int severity;
    const char *type;
    const char *note;

    /*  Exceptions are only accepted on rank 0
     */
    if (exec->ctx->rank != 0) {
        flux_log (h,
                  LOG_ERR,
                  "unexpectedly recieved exception request, ignoring");
        return;
    }

    /*  Unpack exception data
     */
    if (json_unpack (data,
                     "{s:I s:i s:s s:s}",
                     "id", &id,
                     "severity", &severity,
                     "type", &type,
                     "note", &note) < 0) {
        flux_log (h, LOG_ERR, "exec_exception: %s", flux_msg_last_error (msg));
        return;
    }
    flux_log (h,
              LOG_DEBUG,
              "%ju: exception: severity=%d type=%s note=%s",
              (uintmax_t) id,
              severity,
              type,
              note);

    /*  Look up job to get request message
     */
    if (!(job = zhashx_lookup (exec->jobs, &id))) {
        flux_log (h, LOG_ERR, "exec_exception: %ju not found", (uintmax_t) id);
        return;
    }

    /*  Respond to original start request
     */
    if (job->request) {
        if (flux_respond_pack (h,
                               job->request,
                               "{s:s s:{s:i s:s s:s}}",
                               "type", "exception",
                               "data",
                               "severity", severity,
                               "type", type,
                               "note", note) < 0)
            flux_log_error (h, "exec_exception: flux_respond");
    }

    /* Kill job */
    char *ranks = idset_encode (job->subtree_ranks, IDSET_FLAG_RANGE);
    if (severity == 0
        && derp_forward (exec->ctx,
                         "kill",
                         ranks,
                         "{s:I s:i}",
                         "id", job->id,
                         "signal", SIGTERM) < 0) {
        flux_log_error (h, "exec_exception: derp_forward: kill");
    }
    free (ranks);
}

static void timer_cb (flux_reactor_t *r,
                      flux_watcher_t *w,
                      int revents,
                      void *arg)
{
    struct exec *exec = arg;
    struct hello_response *hresp;

    flux_log (exec->ctx->h,
              LOG_DEBUG,
              "sending hello response with %d entries",
              hello_responder_count (exec->hr));

    if ((hresp = hello_responder_pop (exec->hr))) {
        int rc = peer_forward_response (exec->ctx->h,
                                        exec->ctx->peers,
                                        hresp);
        hello_response_destroy (hresp);
        if (rc < 0)
            flux_log_error (exec->ctx->h, "peer_forward_response");
    }
    flux_watcher_stop (w);
}

static void exec_ctx_destroy (struct exec *exec)
{
    if (exec) {
        int saved_errno = errno;
        zhashx_destroy (&exec->jobs);
        idset_destroy (exec->idset);
        hello_responder_destroy (exec->hr);
        flux_watcher_destroy (exec->hr_timer);
        flux_msg_handler_delvec (exec->handlers);
        free (exec);
        errno = saved_errno;
    }
}

static const struct flux_msg_handler_spec htab[] = {
    {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .topic_glob = "derp.start",
        .cb = exec_start,
        .rolemask = 0
    },
    {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .topic_glob = "derp.kill",
        .cb = exec_kill,
        .rolemask = 0
    },
    FLUX_MSGHANDLER_TABLE_END
};

static struct exec * exec_ctx_create (struct derp_ctx *ctx)
{
    struct exec *exec;
    flux_reactor_t *r = flux_get_reactor (ctx->h);

    if (!(exec = calloc (1, sizeof (*exec))))
        return NULL;
    exec->ctx = ctx;
    if (!(exec->jobs = derp_job_hash_create ())
        || !(exec->idset = idset_copy (ctx->peers->idset))
        || !(exec->hr = hello_responder_create ())
        || !(exec->hr_timer = flux_timer_watcher_create (r,
                                                         0.01,
                                                         0.,
                                                         timer_cb,
                                                         exec)))
        goto error;
    if (idset_set (exec->idset, exec->ctx->rank) < 0)
        goto error;
    if (flux_msg_handler_addvec (ctx->h, htab, exec, &exec->handlers) < 0)
        goto error;
    return exec;
error:
    exec_ctx_destroy (exec);
    return NULL;
}

int exec_init (struct derp_ctx *ctx)
{
    struct exec *exec = exec_ctx_create (ctx);
    if (!exec)
        return -1;
    if (derp_register_action (ctx,
                              "state-update",
                              exec_state_update,
                              (flux_free_f) exec_ctx_destroy,
                              exec) < 0
        || derp_register_action (ctx, "kill", derp_exec_kill, NULL, exec) < 0
        || derp_register_notify (ctx, "start", exec_started, exec) < 0
        || derp_register_notify (ctx, "barrier-enter", exec_barrier, exec) < 0
        || derp_register_notify (ctx, "finish", exec_finish, exec) < 0
        || derp_register_notify (ctx, "release", exec_release, exec) < 0
        || derp_register_notify (ctx, "exception", exec_exception, exec) < 0)
        return -1;
    return 0;
}
