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

#include <stdarg.h>

#include "src/common/libutil/errno_safe.h"

#include "hello.h"

struct hello_responder {
    int count;
    struct idset *ranks;
    json_t *jobs;
};

void hello_responder_destroy (struct hello_responder *hr)
{
    if (hr) {
        int saved_errno = errno;
        idset_destroy (hr->ranks);
        json_decref (hr->jobs);
        free (hr);
        errno = saved_errno;
    }
}

struct hello_responder *hello_responder_create ()
{
    struct hello_responder *hr;

    if (!(hr = calloc (1, sizeof (*hr)))
        || !(hr->jobs = json_array ())
        || !(hr->ranks = idset_create (0, IDSET_FLAG_AUTOGROW))) {
        errno = ENOMEM;
        goto error;
    }
    return hr;
error:
    hello_responder_destroy (hr);
    return NULL;
}

int hello_responder_push (struct hello_responder *hr,
                          const char *type,
                          flux_jobid_t id,
                          uint32_t userid,
                          struct idset *ranks)
{
    json_t *job;
    char *ids;

    if (!(ids = idset_encode (ranks, IDSET_FLAG_RANGE))
        || (!(job = json_pack ("{s:I s:i s:s s:s}",
                               "id", id,
                               "userid", userid,
                               "type", type,
                               "ranks", ids)))
        || json_array_append_new (hr->jobs, job) < 0) {
        errno = ENOMEM;
        goto error;
    }

    if (idset_add (hr->ranks, ranks) < 0)
        goto error;
    hr->count++;
    free (ids);
    return 0;
error:
    json_decref (job);
    ERRNO_SAFE_WRAP (free, ids);
    return -1;
}

static int hello_responder_clear (struct hello_responder *hr)
{
    if (json_array_clear (hr->jobs) < 0
        || idset_clear_all (hr->ranks) < 0)
        return -1;
    hr->count = 0;
    return 0;
}

int hello_responder_count (struct hello_responder *hr)
{
    return (int) json_array_size (hr->jobs);
}

void hello_response_destroy (struct hello_response *hresp)
{
    if (hresp && --hresp->refcount == 0) {
        int saved_errno = errno;
        free (hresp->ids);
        idset_destroy (hresp->idset);
        json_decref (hresp->data);
        free (hresp->type);
        free (hresp);
        errno = saved_errno;
    }
}

struct hello_response *hello_response_vpack (const char *type,
                                             const struct idset *idset,
                                             const char *idset_str,
                                             const char *fmt,
                                             va_list ap)
{
    struct hello_response *hresp = NULL;
    json_error_t error;

    if (!type || (!idset && !idset_str) || !fmt) {
        errno = EINVAL;
        return NULL;
    }
    if (!(hresp = calloc (1, sizeof (*hresp))))
        return NULL;
    hresp->refcount = 1;
    hresp->type = strdup (type);

    if (!(hresp->data = json_vpack_ex (&error, JSON_ENCODE_ANY, fmt, ap))) {
        errno = EPROTO;
        goto error;
    }

    /*  One or either of idset and idset_str may have been provided
     */
    if (idset)
        hresp->idset = idset_copy (idset);
    else
        hresp->idset = idset_decode (idset_str);

    if (idset_str)
        hresp->ids = strdup (idset_str);
    else
        hresp->ids = idset_encode (hresp->idset, IDSET_FLAG_RANGE);

    if (!hresp->type || !hresp->ids || !hresp->idset)
        goto error;

    return hresp;
error:
    hello_response_destroy (hresp);
    return NULL;
}

struct hello_response *hello_response_pack (const char *type,
                                            struct idset *idset,
                                            const char *idset_str,
                                            const char *fmt,
                                            ...)
{
    struct hello_response *hresp;
    va_list ap;
    va_start (ap, fmt);
    hresp = hello_response_vpack (type, idset, idset_str, fmt, ap);
    va_end (ap);
    return hresp;
}

struct hello_response *hello_response_incref (struct hello_response *hresp)
{
    hresp->refcount++;
    return hresp;
}

void hello_response_decref (struct hello_response *hresp)
{
    hello_response_destroy (hresp);
}

struct hello_response *hello_responder_pop (struct hello_responder *hr)
{
    struct hello_response *hresp = NULL;
    json_t *jobs;

    if (hr->count == 0)
        return NULL;

    if (!(jobs = json_deep_copy (hr->jobs))) {
        errno = ENOMEM;
        return NULL;
    }
    if (!(hresp = hello_response_pack ("state-update",
                                       hr->ranks,
                                       NULL,
                                       "{s:o}",
                                       "jobs", jobs)))
        return NULL;

    hello_responder_clear (hr);
    return hresp;
}

/*
 * vi: sw=4 ts=4 expandtab
 */
