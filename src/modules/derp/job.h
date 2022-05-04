/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _DERP_JOB_H
#define _DERP_JOB_H

#include <flux/core.h>
#include <flux/idset.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libjob/job.h"

#include "barrier.h"

struct derp_job {
    struct exec *exec;

    flux_jobid_t  id;
    char *        state;
    uint32_t      userid;
    struct idset *ranks;
    struct idset *subtree_ranks;

    struct barrier *barrier;

    struct idset *start_ranks;

    struct idset *finish_ranks;
    int status;

    struct idset *release_ranks;

    const flux_msg_t *request;
    flux_subprocess_t *p;
};

struct derp_job * derp_job_create (flux_jobid_t id,
                                   uint32_t userid,
                                   const char *ranks);

void derp_job_destroy (struct derp_job *job);

zhashx_t *derp_job_hash_create (void);


#endif /* !_DERP_JOB_H */
