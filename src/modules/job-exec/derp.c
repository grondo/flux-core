/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* Flux job derp exec implementation
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <jansson.h>

#include <flux/core.h>
#include <flux/idset.h>

#include "job-exec.h"

struct derpexec {
    struct jobinfo *job;
};

static struct derpexec * derpexec_create (struct jobinfo *job)
{
    struct derpexec *de = calloc (1, sizeof (*de));
    if (de == NULL)
        return NULL;
    de->job = job;
    return (de);
}

static void derpexec_destroy (struct derpexec *de)
{
    free (de);
}

static bool derpexec_enabled (json_t *jobspec)
{
    json_error_t err;
    json_t *o = NULL;

    if (json_unpack_ex (jobspec, &err, 0,
                        "{s:{s?{s?{s?o}}}}",
                        "attributes", "system", "exec",
                        "derp", &o) < 0)
        return false;
    return o != NULL;
}

static int derpexec_init (struct jobinfo *job)
{
    struct derpexec *de = NULL;

    if (!derpexec_enabled (job->jobspec))
        return 0;

    if (!(de = derpexec_create (job))) {
        jobinfo_fatal_error (job, errno, "failed to init derp exec module");
        return -1;
    }
    job->data = (void *) de;
    return 1;
}

static void derp_start_cb (flux_future_t *f, void *arg)
{
    struct jobinfo *job = arg;
    const char *type;
    const char *ranks = NULL;
    int status = -1;

    if (flux_rpc_get_unpack (f,
                             "{s:s s:{s?i s?i}}",
                             "type", &type,
                             "data",
                               "ranks", &ranks,
                               "status", &status) < 0) {
        jobinfo_fatal_error (job, EPROTO, "derp: fail to upack start response");
        return;
    }

    flux_log (flux_future_get_flux (f),
              LOG_DEBUG,
              "job-exec: derp start response type=%s", type);

    if (strcmp (type, "start") == 0)
        jobinfo_started (job);
    else if (strcmp (type, "finish") == 0) {
        jobinfo_tasks_complete (job, resource_set_ranks (job->R), status);
        flux_future_destroy (f);
        return;
    }
    else if (strcmp (type, "exception") == 0) {
        const char *type;
        const char *note;
        int severity;

        if (flux_rpc_get_unpack (f,
                                 "{s:{s:s s:i s:s}}",
                                 "data",
                                 "type", &type,
                                 "severity", &severity,
                                 "note", &note) < 0)
            jobinfo_fatal_error (job, EPROTO, "derp: exception response");
    }
    else if (strcmp (type, "release") == 0) {
        flux_future_destroy (f);
        return;
    }
    flux_future_reset (f);
}

static int derpexec_start (struct jobinfo *job)
{
    int rc = 0;
    if (job->reattach) {
        errno = ENOTSUP;
        rc = -1;
    }
    else {
        flux_future_t *f = NULL;
        const struct idset *idset = resource_set_ranks (job->R);
        char *ranks = idset_encode (idset, IDSET_FLAG_RANGE);

        if (!ranks
            || !(f = flux_rpc_pack (job->h,
                                    "derp.start",
                                    0,
                                    FLUX_RPC_STREAMING,
                                    "{s:I s:i s:s}",
                                    "id", job->id,
                                    "userid", job->userid,
                                    "ranks", ranks))) {
            rc = -1;
        }
        free (ranks);
        if (f)
            flux_future_then (f, -1., derp_start_cb, job);
    }
    return rc;
}

static int derpexec_kill (struct jobinfo *job, int signum)
{
    int rc = 0;
    flux_future_t *f = NULL;
    const struct idset *idset = resource_set_ranks (job->R);
    char *ranks = idset_encode (idset, IDSET_FLAG_RANGE);

    if (!ranks
        || !(f = flux_rpc_pack (job->h,
                                "derp.kill",
                                0,
                                0,
                                "{s:I s:s s:i}",
                                "id", job->id,
                                "ranks", ranks,
                                "signal", signum))) {
        rc = -1;
    }
    free (ranks);
    flux_future_destroy (f);
    return rc;
}

static void derpexec_exit (struct jobinfo *job)
{
    struct derpexec *de = job->data;
    derpexec_destroy (de);
    job->data = NULL;
}

static int derpexec_config (flux_t *h, int argc, char **argv)
{
    return 0;
}

static void derpexec_unload (void)
{
    return;
}

struct exec_implementation derpexec = {
    .name =     "derpexec",
    .config =   derpexec_config,
    .unload =   derpexec_unload,
    .init =     derpexec_init,
    .exit =     derpexec_exit,
    .start =    derpexec_start,
    .kill =     derpexec_kill,
};

/* vi: ts=4 sw=4 expandtab
 */
