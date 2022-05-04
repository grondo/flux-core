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

#include "src/common/libjob/job_hash.h"

#include "job.h"
#include "exec/barrier.h"

void derp_job_destroy (struct derp_job *job)
{
    if (job) {
        int saved_errno = errno;
        idset_destroy (job->ranks);
        idset_destroy (job->start_ranks);
        idset_destroy (job->finish_ranks);
        idset_destroy (job->release_ranks);
        idset_destroy (job->subtree_ranks);
        barrier_destroy (job->barrier);
        flux_msg_decref (job->request);
        flux_subprocess_destroy (job->p);
        free (job);
        errno = saved_errno;
    }
}

struct derp_job * derp_job_create (flux_jobid_t id,
                                   uint32_t userid,
                                   const char *ranks)
{
    struct derp_job *job = calloc (1, sizeof (*job));
    if (!job
        || !(job->ranks = idset_decode (ranks))
        || !(job->barrier = barrier_create ())
        || !(job->start_ranks = idset_create (0, IDSET_FLAG_AUTOGROW))
        || !(job->finish_ranks = idset_create (0, IDSET_FLAG_AUTOGROW))
        || !(job->release_ranks = idset_create (0, IDSET_FLAG_AUTOGROW)))
        goto error;
    job->id = id;
    job->userid = userid;
    return job;
error:
    derp_job_destroy (job);
    return NULL;
}

/*  zhashx_destructor_fn signature
 */
static void derp_job_destructor (void **item)
{
    if (item) {
        derp_job_destroy (*item);
        *item = NULL;
    }
}

zhashx_t *derp_job_hash_create (void)
{
    zhashx_t *jobs = job_hash_create ();
    if (!jobs)
        return NULL;
    zhashx_set_destructor (jobs, derp_job_destructor);
    return jobs;
}


struct derp_job *derp_job_hash_add (zhashx_t *jobs,
                                    flux_jobid_t id,
                                    uint32_t userid,
                                    const char *ranks)
{
    struct derp_job *job = zhashx_lookup (jobs, &id);
    if (job) {
        errno = EEXIST;
        return NULL;
    }
    if (!(job = derp_job_create (id, userid, ranks)))
        return NULL;
    (void) zhashx_insert (jobs, &job->id, job);
    return job;
}

struct derp_job *derp_job_hash_lookup (zhashx_t *jobs, flux_jobid_t id)
{
    return zhashx_lookup (jobs, &id);
}

void derp_job_hash_delete (zhashx_t *jobs, flux_jobid_t id)
{
    zhashx_delete (jobs, &id);
}

