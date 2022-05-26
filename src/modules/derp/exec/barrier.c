/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <flux/core.h>
#include <flux/idset.h>

#include "barrier.h"

static void msg_destructor (void **item) {
    if (item) {
        flux_msg_decref (*item);
        *item = NULL;
    }
}

void barrier_destroy (struct barrier *barrier)
{
    if (barrier) {
        int saved_errno = errno;
        zlistx_destroy (&barrier->requests);
        idset_destroy (barrier->ranks);
        free (barrier);
        errno = saved_errno;
    }
}

struct barrier *barrier_create (void)
{
    struct barrier *barrier;

    if (!(barrier = calloc (1, sizeof (*barrier)))
        || !(barrier->ranks = idset_create (0, IDSET_FLAG_AUTOGROW))
        || !(barrier->requests = zlistx_new ()))
        goto error;

    zlistx_set_destructor (barrier->requests, msg_destructor);

    return barrier;
error:
    barrier_destroy (barrier);
    return NULL;
}

void barrier_reset (struct barrier *barrier)
{
    if (barrier) {
        barrier->sequence++;
        idset_clear_all (barrier->ranks);
        zlistx_purge (barrier->requests);
    }
}

int barrier_enter (struct barrier *barrier,
                   const flux_msg_t *msg)
{
    int rc = -1;
    const char *ranks;
    int sequence;
    struct idset *idset = NULL;

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:{s:s s:i}}",
                             "data",
                               "ranks", &ranks,
                               "seq", &sequence) < 0)
        return -1;
    if (sequence != barrier->sequence) {
        errno = EINVAL;
        goto error;
    }
    if (!(idset = idset_decode (ranks))
        || idset_add (barrier->ranks, idset) < 0)
        goto error;
    if (!zlistx_add_end (barrier->requests, (flux_msg_t *) msg))
        goto error;
    flux_msg_incref (msg);
    rc = 0;
error:
    idset_destroy (idset);
    return rc;
}

int barrier_enter_local (struct barrier *barrier, int rank)
{
    return idset_set (barrier->ranks, rank);
}

flux_future_t * barrier_notify (flux_t *h,
                                flux_jobid_t id,
                                struct barrier *barrier)
{
    char *ranks;
    flux_future_t *f;

    if (!(ranks = idset_encode (barrier->ranks, IDSET_FLAG_RANGE)))
        return NULL;
    f = flux_rpc_pack (h,
                       "derp.notify",
                       FLUX_NODEID_UPSTREAM,
                       0,
                       "{s:s s:{s:I s:s s:i}}",
                       "type", "barrier-enter",
                       "data",
                         "id", id,
                         "ranks", ranks,
                         "seq", barrier->sequence);
    if (!f)
        flux_log_error (h, "barrier_notify: flux_rpc_pack");
    free (ranks);
    return f;
}

int barrier_respond_all (flux_t *h, struct barrier *barrier)
{
    int rc = 0;
    const flux_msg_t *msg = zlistx_first (barrier->requests);
    while (msg) {
        if (flux_respond (h, msg, NULL) < 0) {
            rc = -1;
            flux_log_error (h, "barrier_notify: flux_respond");
        }
        msg = zlistx_next (barrier->requests);
    }
    return rc;
}

/* vi: ts=4 sw=4 expandtab
 */
