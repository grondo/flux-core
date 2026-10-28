/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FLUX_JOB_MANAGER_RUNTIME_H
#define _FLUX_JOB_MANAGER_RUNTIME_H

#include <jansson.h>
#include <flux/core.h>

#include "queues.h"

/* The KVS key holding the runtime queues, and the version this code
 * writes.  The key is the only persistent record of a runtime queue:
 * the job-manager writes it, and the resource module reads it when it
 * defines R (see queue_membership_apply_runtime()).
 */
#define RUNTIME_QUEUES_KEY "queues.runtime"
#define RUNTIME_QUEUES_VERSION 1

/* Build the key object from the runtime queues in 'queues', with one
 * proposed change applied.  'queues' is not modified: the change is
 * written to the key before it is applied in memory, so that the key
 * never describes less than R does.
 *
 * To propose an addition or a replacement, pass 'name' and 'entry'; the
 * entry is the per-queue object described in runtime.c.  To propose a
 * removal, pass 'remove_name'.  Pass NULL for both to encode the
 * current state unchanged, which is how a failed operation puts the key
 * back.
 *
 * Returns a new object (caller decrefs), or NULL on error with errno
 * set.
 */
json_t *runtime_queues_encode (struct queues *queues,
                               const char *name,
                               json_t *entry,
                               const char *remove_name);

/* Build the entry for queue 'q' as it is now, optionally overriding the
 * started or enabled bit.  Pass -1 for an override to keep the queue's
 * current value.  Returns a new object (caller decrefs), or NULL on
 * error with errno set.
 */
json_t *runtime_queue_entry (struct queue *q,
                             int started,
                             int enabled,
                             const char *reason);

/* Commit 'o' to the runtime queues key.  Returns a future, or NULL on
 * error with errno set.  The caller waits for the commit before it
 * sends any resource.property-update: see the write-ahead note in
 * runtime.c.
 */
flux_future_t *runtime_queues_commit (flux_t *h, json_t *o);

/* Look up the runtime queues key synchronously.  On success returns 0
 * and sets '*op' to a new reference (caller decrefs) or to NULL if the
 * key is absent or unusable.  Returns -1 on error with errno set.
 */
int runtime_queues_lookup (flux_t *h, json_t **op);

#endif /* !_FLUX_JOB_MANAGER_RUNTIME_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
