/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _DERP_BARRIER_H
#define _DERP_BARRIER_H

#include <flux/core.h>
#include <flux/idset.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libjob/job.h"


struct barrier {
    zlistx_t *requests;
    int sequence;
    struct idset *ranks;
};


struct barrier *barrier_create ();

void barrier_destroy (struct barrier *barrier);

void barrier_reset (struct barrier *barrier);

int barrier_enter (struct barrier *barrier, const flux_msg_t *msg);

int barrier_enter_local (struct barrier *barrier, int rank);

flux_future_t * barrier_notify (flux_t *h,
                                flux_jobid_t id,
                                struct barrier *barrier);

int barrier_respond_all (flux_t *h, struct barrier *barrier);


#endif /* !_DERP_BARRIER_H */
