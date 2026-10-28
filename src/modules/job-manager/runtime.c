/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* runtime.c - persistent record of queues created at runtime
 *
 * A runtime queue (job-manager.queue-add) is recorded in one KVS key,
 * queues.runtime:
 *
 *   {"version":1,
 *    "queues":{"dat":{"hosts":"node[2-3]",
 *                     "enabled":true,
 *                     "started":false,
 *                     "disable_reason":"...",
 *                     "stop_reason":"..."}}}
 *
 * 'hosts' is stored exactly as the admin gave it, a hostlist rather than
 * an idset, so that it means the same thing after a restart that
 * renumbers ranks.
 *
 * The job-manager is the only writer.  The resource module reads the key
 * when it defines R and derives each queue's membership property from
 * it, so a runtime queue survives a reload of either module without the
 * two having to talk to each other.
 *
 * WRITE-AHEAD.  Every admin operation commits the key and waits for the
 * commit to complete BEFORE it sends the resource.property-update that
 * changes R.  The key therefore never describes less than R does.  The
 * reverse -- a key that describes more than R -- is harmless and
 * self-correcting: the resource module applies the key on its next load.
 * If the property update fails, the operation writes the key back from
 * current in-memory state before it responds.
 *
 * Runtime queues are deliberately absent from the job-manager checkpoint
 * (checkpoint.job-manager).  This key is their only record, so there is
 * one writer and no way for two records to disagree.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>

#include "src/common/libutil/errno_safe.h"
#include "ccan/str/str.h"

#include "runtime.h"

json_t *runtime_queue_entry (struct queue *q,
                             int started,
                             int enabled,
                             const char *reason)
{
    const char *hosts = queue_hosts (q);
    json_t *entry;

    if (started < 0)
        started = queue_is_started (q) ? 1 : 0;
    if (enabled < 0)
        enabled = queue_is_enabled (q) ? 1 : 0;
    if (!(entry = json_pack ("{s:s s:b s:b}",
                             "hosts", hosts ? hosts : "",
                             "enabled", enabled,
                             "started", started))) {
        errno = ENOMEM;
        return NULL;
    }
    /* A reason belongs to whichever bit the caller is clearing.  The
     * queue's own reason is kept when the caller passes none, so that a
     * start does not drop the reason a later stop would restore.
     */
    if (!reason)
        reason = enabled ? queue_stop_reason (q) : queue_disable_reason (q);
    if (reason) {
        const char *key = enabled ? "stop_reason" : "disable_reason";
        if (json_object_set_new (entry, key, json_string (reason)) < 0) {
            json_decref (entry);
            errno = ENOMEM;
            return NULL;
        }
    }
    return entry;
}

json_t *runtime_queues_encode (struct queues *queues,
                               const char *name,
                               json_t *entry,
                               const char *remove_name)
{
    json_t *o;
    json_t *table;
    struct queue *q;
    zlistx_t *names = NULL;
    const char *qname;

    if (!(o = json_pack ("{s:i s:{}}",
                         "version", RUNTIME_QUEUES_VERSION,
                         "queues"))) {
        errno = ENOMEM;
        return NULL;
    }
    table = json_object_get (o, "queues");

    if (!(names = queues_list_names (queues)))
        goto error;
    qname = zlistx_first (names);
    while (qname) {
        if ((q = queues_lookup (queues, qname, NULL))
            && queue_is_runtime (q)
            && !(remove_name && streq (qname, remove_name))
            && !(name && streq (qname, name))) {
            json_t *cur;
            if (!(cur = runtime_queue_entry (q, -1, -1, NULL))
                || json_object_set_new (table, qname, cur) < 0) {
                json_decref (cur);
                errno = ENOMEM;
                goto error;
            }
        }
        qname = zlistx_next (names);
    }
    zlistx_destroy (&names);

    /* The proposed entry goes in last, so it replaces rather than
     * merges with any entry of the same name above.
     */
    if (name && entry) {
        if (json_object_set (table, name, entry) < 0) {
            errno = ENOMEM;
            goto error;
        }
    }
    return o;
error:
    zlistx_destroy (&names);
    ERRNO_SAFE_WRAP (json_decref, o);
    return NULL;
}

flux_future_t *runtime_queues_commit (flux_t *h, json_t *o)
{
    flux_kvs_txn_t *txn;
    flux_future_t *f = NULL;

    if (!(txn = flux_kvs_txn_create ()))
        return NULL;
    if (flux_kvs_txn_pack (txn, 0, RUNTIME_QUEUES_KEY, "O", o) < 0)
        goto done;
    f = flux_kvs_commit (h, NULL, 0, txn);
done:
    ERRNO_SAFE_WRAP (flux_kvs_txn_destroy, txn);
    return f;
}

int runtime_queues_lookup (flux_t *h, json_t **op)
{
    flux_future_t *f;
    json_t *o;
    json_t *queues;
    int version;

    *op = NULL;
    if (!(f = flux_kvs_lookup (h, NULL, 0, RUNTIME_QUEUES_KEY)))
        return -1;
    /* An absent key is the common case.  An unreadable one is reported
     * as absent rather than fatal: the instance must still come up, and
     * the next save rewrites the key.
     */
    if (flux_kvs_lookup_get_unpack (f, "o", &o) < 0) {
        if (errno != ENOENT)
            flux_log_error (h, "%s: lookup failed", RUNTIME_QUEUES_KEY);
        flux_future_destroy (f);
        return 0;
    }
    if (json_unpack (o,
                     "{s:i s:o}",
                     "version", &version,
                     "queues", &queues) < 0
        || !json_is_object (queues)) {
        flux_log (h, LOG_ERR, "%s: malformed key, ignoring", RUNTIME_QUEUES_KEY);
        flux_future_destroy (f);
        return 0;
    }
    if (version != RUNTIME_QUEUES_VERSION) {
        flux_log (h,
                  LOG_ERR,
                  "%s: unknown version %d, ignoring",
                  RUNTIME_QUEUES_KEY,
                  version);
        flux_future_destroy (f);
        return 0;
    }
    *op = json_incref (queues);
    flux_future_destroy (f);
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
