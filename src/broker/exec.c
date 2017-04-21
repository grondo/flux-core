/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/param.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <assert.h>
#include <flux/core.h>

#include "src/common/libsubprocess/zio.h"
#include "src/common/libsubprocess/subprocess.h"
#include "src/common/libutil/log.h"
#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/zsigcert.h"
#include "src/common/libutil/xzmalloc.h"

#include "attr.h"
#include "exec.h"

typedef struct {
    flux_t *h;
    struct subprocess_manager *sm;
    uint32_t rank;
    attr_t *attrs;
    uid_t owner;
    zsigcert_t *cert;
} exec_t;

typedef struct {
    exec_t *x;
    flux_msg_t *msg;
    uid_t userid;
} proc_info_t;


static proc_info_t *
proc_info_create (exec_t *x, const flux_msg_t *msg, uid_t userid)
{
    proc_info_t *pi = xzmalloc (sizeof (*pi));
    pi->x = x;
    pi->msg = flux_msg_copy (msg, true);
    pi->userid = userid;
    return (pi);
}

static void
proc_info_destroy (proc_info_t *pi)
{
    pi->x = NULL;
    flux_msg_destroy (pi->msg);
    pi->msg = NULL;
    pi->userid = -1;
    free (pi);
}

static json_object *
subprocess_json_resp (exec_t *x, struct subprocess *p)
{
    json_object *resp = Jnew ();

    assert (x != NULL);
    assert (resp != NULL);

    Jadd_int (resp, "rank", x->rank);
    Jadd_int (resp, "pid", subprocess_pid (p));
    Jadd_str (resp, "state", subprocess_state_string (p));
    return (resp);
}

static int child_exit_handler (struct subprocess *p)
{
    int n;
    proc_info_t *pi = subprocess_get_context (p, "proc_info");
    exec_t *x = pi->x;
    flux_msg_t *msg = pi->msg;
    json_object *resp;

    assert (x != NULL);
    assert (msg != NULL);

    resp = subprocess_json_resp (x, p);
    Jadd_int (resp, "status", subprocess_exit_status (p));
    Jadd_int (resp, "code", subprocess_exit_code (p));
    if ((n = subprocess_signaled (p)))
        Jadd_int (resp, "signal", n);
    if ((n = subprocess_exec_error (p)))
        Jadd_int (resp, "exec_errno", n);

    flux_respond (x->h, msg, 0, Jtostr (resp));
    json_object_put (resp);

    proc_info_destroy (pi);
    subprocess_destroy (p);

    return (0);
}

static int subprocess_io_cb (struct subprocess *p, const char *json_str)
{
    proc_info_t *pi = subprocess_get_context (p, "proc_info");
    exec_t *x = pi->x;
    flux_msg_t *orig = pi->msg;
    json_object *o = NULL;
    int rc = -1;

    assert (x != NULL);
    assert (orig != NULL);

    if (!(o = Jfromstr (json_str))) {
        errno = EPROTO;
        goto done;
    }
    /* Add this rank */
    Jadd_int (o, "rank", x->rank);
    rc = flux_respond (x->h, orig, 0, Jtostr (o));
done:
    Jput (o);
    return (rc);
}

static struct subprocess *
subprocess_get_pid (struct subprocess_manager *sm, int pid)
{
    struct subprocess *p = NULL;
    p = subprocess_manager_first (sm);
    while (p) {
        if (pid == subprocess_pid (p))
            return (p);
        p = subprocess_manager_next (sm);
    }
    return (NULL);
}

static void write_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg)
{
    exec_t *x = arg;
    json_object *request = NULL;
    json_object *o = NULL;
    const char *json_str;
    int pid;
    int errnum = EPROTO;

    if (flux_request_decode (msg, NULL, &json_str) < 0) {
        errnum = errno;
        goto out;
    }

    if (json_str
        && (request = Jfromstr (json_str))
        && Jget_int (request, "pid", &pid)
        && Jget_obj (request, "stdin", &o)) {
        int len;
        void *data = NULL;
        bool eof;
        struct subprocess *p;
        uint32_t userid;
        proc_info_t *pi;

        /* XXX: We use zio_json_decode() here for convenience. Probably
         *  this should be bubbled up as a subprocess IO json spec with
         *  encode/decode functions.
         */
        if ((len = zio_json_decode (Jtostr (o), &data, &eof)) < 0)
            goto out;
        if (!(p = subprocess_get_pid (x->sm, pid)) ||
            !(pi = subprocess_get_context (p, "proc_info"))) {
            errnum = ENOENT;
            free (data);
            goto out;
        }
        if (flux_msg_get_userid (msg, &userid) < 0) {
            errnum = EINVAL;
            free (data);
            goto out;
        }
        if ((uid_t) userid != pi->userid) {
            errnum = EACCES;
            free (data);
            goto out;
        }
        if (subprocess_write (p, data, len, eof) < 0) {
            errnum = errno;
            free (data);
            goto out;
        }
        free (data);
        errnum = 0;
    }
out:
    if (flux_respondf (h, msg, "{ s:i }", "code", errnum) < 0)
        flux_log_error (h, "write_request_cb: flux_respondf");
    if (request)
        json_object_put (request);
}


static void signal_request_respond (flux_t *h, const flux_msg_t *msg,
    int errnum)
{
    if (flux_respondf (h, msg, "{s:i}", "code", errnum) < 0)
        flux_log_error (h, "signal_request_respond: flux_respondf");
}

static int imp_kill_completion (struct subprocess *p)
{
    int errnum = -EACCES;
    proc_info_t *pi = subprocess_get_context (p, "proc_info");

    /*  If proc_info object was registered with this subprocess, then
     *   there is a signal request message to which we must reply.
     */
    if (pi) {
        if (subprocess_exit_code (p) == 0)
            errnum = 0;
        else
            flux_log (pi->x->h, LOG_ERR, "flux mock-imp kill: %s",
                    subprocess_exit_string (p));
        signal_request_respond (pi->x->h, pi->msg, errnum);
        proc_info_destroy (pi);
    }

    subprocess_destroy (p);
    return (0);
}

static int imp_kill_process (exec_t *x, const flux_msg_t *msg, pid_t pid)
{
    proc_info_t *pi;
    char *args[] = { "flux", "mock-imp", "kill", NULL};
    int argc = 3;
    struct subprocess *p;
    char pidstr[64];
    int n;
    int rc = -1;

    n = snprintf (pidstr, sizeof (pidstr), "%lu", (unsigned long) pid);
    if ((n <= 0) || (n >= sizeof (pidstr))) {
        flux_log_error (x->h, "failed to convert pid to string");
        return (-1);
    }

    if (!(p = subprocess_create (x->sm))) {
        flux_log_error (x->h, "subprocess_create");
        goto done;
    }
    if (subprocess_set_args (p, argc, args) < 0) {
        flux_log_error (x->h, "subprocess_set_args");
        goto done;
    }
    if (subprocess_argv_append (p, pidstr) < 0) {
        flux_log_error (x->h, "subprocess_argv_append");
        goto done;
    }

    // Propagate current environment
    subprocess_set_environ (p, environ);

    /*  Record proc_info_t if this call to imp_kill_process() was the
     *   result of a message (i.e. msg != NULL), so that the completion
     *   handler can issue a reply.
     */
    if (msg) {
        proc_info_t *pi = proc_info_create (x, msg, -1);
        if (pi == NULL)
            goto done;
        subprocess_set_context (p, "proc_info", (void *) pi);
    }
    subprocess_add_hook (p, SUBPROCESS_COMPLETE, imp_kill_completion);

    if (subprocess_run (p) < 0) {
        flux_log_error (x->h, "subprocess_argv_append");
        goto done;
    }
    return (0);
done:
    if (p)
        subprocess_destroy (p);
    return (rc);
}

static void signal_request_cb (flux_t *h, flux_msg_handler_t *w,
                               const flux_msg_t *msg, void *arg)
{
    exec_t *x = arg;
    int pid;
    int errnum = EPROTO;
    int signum;
    struct subprocess *p;
    uint32_t userid;

    if (flux_request_decodef (msg, NULL, "{ s:i }", "pid", &pid) < 0) {
        errnum = errno;
        goto out;
    }
    if (flux_request_decodef (msg, NULL, "{ s:i }", "signum", &signum) < 0)
        signum = SIGTERM;
    if (flux_msg_get_userid (msg, &userid) < 0) {
        errnum = errno;
        goto out;
    }
    p = subprocess_manager_first (x->sm);
    while (p) {
        if (pid == subprocess_pid (p)) {
            proc_info_t *pi = subprocess_get_context (p, "proc_info");
            errnum = 0;
            /* Send signal to entire process group */
            if (pi->userid == x->owner) {
                if (kill (-pid, signum) < 0)
                    errnum = errno;
                goto out;
            }
            // Either instance owner or process owner can request signal:
            else if (userid == pi->userid || userid == x->owner) {
                /* Asynchronously kill process via IMP */
                if (imp_kill_process (pi->x, msg, -pid) < 0)
                    flux_log_error (h, "imp_kill_process");
                /* return immediately, response will be sent after
                 *  subprocess completion.
                 */
                return;
            }
            else {
                errnum = EACCES;
                goto out;
            }
        }
        p = subprocess_manager_next (x->sm);
    }
out:
    signal_request_respond (h, msg, errnum);
}

static int do_setpgrp (struct subprocess *p)
{
    if (setpgrp () < 0)
        fprintf (stderr, "setpgrp: %s", strerror (errno));
    return (0);
}

static struct subprocess *
multiuser_subprocess (exec_t *x, uid_t userid, const char *J)
{
    struct subprocess *p = NULL;
    json_object *o = NULL;
    json_object *q = NULL;
    const char *Jsig;
    char *R = NULL;
    int argc = 3;
    char *args[] = { "flux", "mock-imp", "run", NULL };

    if (!x->cert) {
        errno = EACCES;
        return (NULL);
    }

    if (!J || !(o = Jfromstr (J)) || !Jget_str (o, "sig", &Jsig)) {
        errno = EPROTO;
        return (NULL);
    }

    /* Create and sign R */
    q = Jnew ();
    Jadd_int (q, "userid", userid);
    Jadd_str (q, "J", Jsig);

    if (zsigcert_sign_json (x->cert, Jtostr (q), &R) < 0)
        goto done;

    if (!(p = subprocess_create (x->sm))) {
        flux_log_error (x->h, "subprocess_create");
        goto done;
    }
    if (subprocess_set_args (p, argc, args) < 0) {
        flux_log_error (x->h, "subprocess_set_args");
        goto done;
    }

    // Propagate current environment
    subprocess_set_environ (p, environ);

    subprocess_write (p, (void *) R, strlen (R), false);
    subprocess_write (p, (void *) J, strlen (J), false);
done:
    Jput (o);
    Jput (q);
    free (R);
    return (p);
}

static struct subprocess *
singleuser_subprocess (exec_t *x, const char *json_str)
{
    int i, argc;
    json_object *o = NULL;
    json_object *request = NULL;
    struct subprocess *p = NULL;

    if (!json_str
        || !(request = Jfromstr (json_str))
        || !json_object_object_get_ex (request, "cmdline", &o)
        || o == NULL
        || (json_object_get_type (o) != json_type_array)) {
        errno = EPROTO;
        goto out_free;
    }

    if ((argc = json_object_array_length (o)) < 0) {
        errno = EPROTO;
        goto out_free;
    }

    p = subprocess_create (x->sm);

    for (i = 0; i < argc; i++) {
        json_object *ox = json_object_array_get_idx (o, i);
        if (json_object_get_type (ox) == json_type_string)
            subprocess_argv_append (p, json_object_get_string (ox));
    }

    if (json_object_object_get_ex (request, "env", &o) && o != NULL) {
        json_object_iter iter;
        json_object_object_foreachC (o, iter) {
            const char *val = json_object_get_string (iter.val);
            if (val != NULL)
                subprocess_setenv (p, iter.key, val, 1);
        }
    }
    else
        subprocess_set_environ (p, environ);

    if (json_object_object_get_ex (request, "cwd", &o) && o != NULL) {
        const char *dir = json_object_get_string (o);
        if (dir != NULL)
            subprocess_set_cwd (p, dir);
    }

out_free:
    Jput (o);
    return (p);
}


/*
 *  Create a subprocess described in the msg argument.
 */
static void exec_request_cb (flux_t *h, flux_msg_handler_t *w,
                             const flux_msg_t *msg, void *arg)
{
    exec_t *x = arg;
    json_object *response = NULL;
    const char *json_str;
    struct subprocess *p;
    const char *local_uri;
    uid_t userid;
    proc_info_t *pi;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto out_free;

    if (flux_request_decode (msg, NULL, &json_str) < 0)
        goto out_free;

    if (userid == x->owner)
        p = singleuser_subprocess (x, json_str);
    else
        p = multiuser_subprocess (x, userid, json_str);

    if (p == NULL)
        goto out_free;

    /*
     *  Override key FLUX environment variables in env array
     */
    if (attr_get (x->attrs, "local-uri", &local_uri, NULL) < 0)
        log_err_exit ("%s: local_uri is not set", __FUNCTION__);
    subprocess_setenv (p, "FLUX_URI", local_uri, 1);

    /*
     *  Set common context and hooks
     */
    if (!(pi = proc_info_create (x, msg, userid)))
        goto out_free;
    subprocess_set_context (p, "proc_info", (void *) pi);
    subprocess_add_hook (p, SUBPROCESS_COMPLETE, child_exit_handler);
    subprocess_add_hook (p, SUBPROCESS_PRE_EXEC, do_setpgrp);

    subprocess_set_io_callback (p, subprocess_io_cb);

    if (subprocess_fork (p) < 0) {
        /*
         *  Fork error, respond directly to exec client with error
         *   (There will be no subprocess to reap)
         */
        (void) flux_respond (h, msg, errno, NULL);
        goto out_free;
    }

    if (subprocess_exec (p) >= 0) {
        /*
         *  Send response, destroys original msg.
         *   For "Exec Failure" allow that state to be transmitted
         *   to caller on completion handler (which will be called
         *   immediately)
         */
        response = subprocess_json_resp (x, p);
        flux_respond (h, msg, 0, Jtostr (response));
    }
out_free:
    if (response)
        json_object_put (response);
}

static char *subprocess_sender (struct subprocess *p)
{
    char *sender;
    proc_info_t *pi = subprocess_get_context (p, "proc_info");
    if (!pi || !pi->msg || flux_msg_get_route_first (pi->msg, &sender) < 0)
        return NULL;
    return (sender);
}

int exec_terminate_subprocesses_by_uuid (flux_t *h, const char *id)
{
    exec_t *x = flux_aux_get (h, "flux::exec");

    struct subprocess *p = subprocess_manager_first (x->sm);
    while (p) {
        char *sender;
        if ((sender = subprocess_sender (p))) {
            pid_t pid;
            if ((strcmp (id, sender) == 0)
               && ((pid = subprocess_pid (p)) > (pid_t) 0)) {
                /* Kill process group for subprocess p */
                flux_log (x->h, LOG_INFO,
                          "Terminating PGRP %ld", (unsigned long) pid);
                if (kill (-pid, SIGKILL) < 0)
                    flux_log_error (x->h, "killpg");
            }
            free (sender);
        }
        p = subprocess_manager_next (x->sm);
    }
    return (0);
}

static json_object *subprocess_json_info (struct subprocess *p)
{
    int i;
    char buf [MAXPATHLEN];
    const char *cwd;
    char *sender = NULL;
    json_object *o = Jnew ();
    json_object *a = Jnew_ar ();

    Jadd_int (o, "pid", subprocess_pid (p));
    for (i = 0; i < subprocess_get_argc (p); i++) {
        Jadd_ar_str (a, subprocess_get_arg (p, i));
    }
    /*  Avoid shortjson here so we don't take
     *   unnecessary reference to 'a'
     */
    json_object_object_add (o, "cmdline", a);
    if ((cwd = subprocess_get_cwd (p)) == NULL)
        cwd = getcwd (buf, MAXPATHLEN-1);
    Jadd_str (o, "cwd", cwd);
    if ((sender = subprocess_sender (p))) {
        Jadd_str (o, "sender", sender);
        free (sender);
    }
    return (o);
}

static void ps_request_cb (flux_t *h, flux_msg_handler_t *w,
                           const flux_msg_t *msg, void *arg)
{
    struct subprocess *p;
    exec_t *x = arg;
    json_object *out = Jnew ();
    json_object *procs = Jnew_ar ();

    Jadd_int (out, "rank", x->rank);

    p = subprocess_manager_first (x->sm);
    while (p) {
        json_object *o = subprocess_json_info (p);
        /* Avoid shortjson here so we don't take an unnecessary
         *  reference to 'o'.
         */
        json_object_array_add (procs, o);
        p = subprocess_manager_next (x->sm);
    }
    json_object_object_add (out, "procs", procs);
    if (flux_respond (h, msg, 0, Jtostr (out)) < 0)
        flux_log_error (h, "%s: flux_respond", __FUNCTION__);
    Jput (out);
}

static struct flux_msg_handler_spec handlers[] = {
    { FLUX_MSGTYPE_REQUEST, "cmb.exec",           exec_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "cmb.exec.signal",    signal_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "cmb.exec.write",     write_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "cmb.processes",      ps_request_cb, 0, NULL },
    FLUX_MSGHANDLER_TABLE_END,
};

static void exec_finalize (void *arg)
{
    exec_t *x = arg;
    free (x);
    flux_msg_handler_delvec (handlers);
}

static void
flux_msg_handler_vec_allow_rolemask (struct flux_msg_handler_spec *spec,
                                     uint32_t mask)
{
    while (spec->w != NULL) {
        flux_msg_handler_allow_rolemask (spec->w, mask);
        spec++;
    }
}

/*
 *  Load cert for userid. Lifted from zsigcert.c
 */
static zsigcert_t *load_cert (flux_t *h, uint32_t userid)
{
    char path[PATH_MAX];
    struct passwd *pw;
    zsigcert_t *cert;

    if (!(pw = getpwuid (userid))) {
        flux_log_error (h, "can't find HOME for uid %ju", (uintmax_t) userid);
        return NULL;
    }
    snprintf (path, sizeof (path),
              "%s/.flux/curve/signature_secret", pw->pw_dir);

    if (!(cert = zsigcert_load (path))) {
        flux_log_error (h, "zsigcert_load");
        return NULL;
    }
    return cert;
}

int exec_initialize (flux_t *h, struct subprocess_manager *sm,
                     uint32_t rank, attr_t *attrs)
{
    exec_t *x = calloc (1, sizeof (*x));
    if (!x) {
        errno = ENOMEM;
        return -1;
    }
    x->owner = getuid ();
    x->h = h;
    x->sm = sm;
    x->rank = rank;
    x->attrs = attrs;
    if (!(x->cert = load_cert (h, x->owner)))
        flux_log_error (h, "no signing cert available, no multi-user support");
    if (flux_msg_handler_addvec (h, handlers, x) < 0) {
        free (x);
        return -1;
    }
    flux_msg_handler_vec_allow_rolemask (handlers, FLUX_ROLE_USER);
    flux_aux_set (h, "flux::exec", x, exec_finalize);
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
