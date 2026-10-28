/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* queue_membership.c - apply queue membership properties to R
 *
 * A configured queue that declares 'hosts' (RFC 33) has its membership
 * property "+NAME" (an RFC 20 instance-local property) applied to the
 * corresponding ranks of the resource object R at module load time. See
 * queue_membership.h for why these are derived here rather than obtained
 * from the job-manager, the authority for queue configuration.
 *
 * queue_membership_update() applies properties that do not come from the
 * configuration, for a queue created at runtime. It is the same splice of
 * execution.properties, driven by explicit property objects rather than by
 * [queues], so the two share the rlist handling below.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>

#include "src/common/librlist/rlist.h"
#include "src/common/libidset/idset.h"
#include "src/common/libhostlist/hostlist.h"
#include "src/common/libutil/errno_safe.h"
#include "src/common/libutil/errprintf.h"
#include "src/common/libutil/jpath.h"
#include "src/common/libfluxutil/conf_policy.h"
#include "ccan/str/str.h"

#include "queue_membership.h"

/* Resolve a queue's 'hosts' value to an idset string of R ranks. 'hosts' is
 * either the literal "all" (every rank in 'rl') or an RFC 29 hostlist. An
 * unknown host or an empty result is a fatal error. Caller frees the result.
 */
static char *hosts_to_idset_string (struct rlist *rl,
                                    const char *name,
                                    const char *hosts,
                                    bool allow_idset,
                                    flux_error_t *errp)
{
    struct idset *ranks = NULL;
    char *s = NULL;

    /* An RFC 22 idset of ranks is tried first where allowed, as
     * resolve_targets() does for a property update, so that the two
     * paths accept the same 'targets' spelling.  A hostlist cannot be
     * mistaken for an idset: idset_decode() accepts only digits,
     * commas, and dashes.
     */
    if (streq (hosts, "all"))
        ranks = rlist_ranks (rl);
    else {
        if (allow_idset)
            ranks = idset_decode (hosts);
        if (!ranks)
            ranks = rlist_hosts_to_ranks (rl, hosts, errp);
    }
    if (!ranks) {
        /* rlist_hosts_to_ranks() already set errp for the hostlist case. */
        if (streq (hosts, "all"))
            errprintf (errp, "queue %s: no resources configured", name);
        return NULL;
    }
    if (idset_count (ranks) == 0) {
        errprintf (errp, "queue %s: hosts '%s' is an empty set", name, hosts);
        errno = EINVAL;
        goto done;
    }
    if (!(s = idset_encode (ranks, IDSET_FLAG_RANGE)))
        errprintf (errp, "queue %s: idset_encode: %s", name, strerror (errno));
done:
    idset_destroy (ranks);
    return s;
}

/* True if any queue in 'queues' declares 'hosts', i.e. there is a membership
 * property to derive.
 */
static bool queue_membership_configured (json_t *queues)
{
    const char *name;
    json_t *entry;

    json_object_foreach (queues, name, entry) {
        json_t *hosts = json_object_get (entry, "hosts");
        if (hosts && json_is_string (hosts))
            return true;
    }
    return false;
}

/* Return a copy of 'R' that shares everything except execution.properties,
 * which is duplicated so queue_membership_apply() can modify it without
 * touching the original.
 *
 * Only the objects along the path to that key are copied, so R's bulk --
 * notably 'scheduling', which carries the JGF on a system instance -- is
 * shared rather than duplicated. The caller must therefore not modify the
 * result's other deep keys.
 */
json_t *queue_membership_copy (json_t *R)
{
    json_t *cpy = NULL;
    json_t *execution = NULL;
    json_t *props = NULL;
    json_t *orig_props;

    if (!(cpy = json_copy (R))
        || !(execution = json_copy (json_object_get (R, "execution"))))
        goto nomem;
    /* execution.properties is absent until a membership property is
     * applied, in which case apply() creates it in the copy.
     */
    if ((orig_props = json_object_get (execution, "properties"))) {
        if (!(props = json_copy (orig_props))
            || json_object_set (execution, "properties", props) < 0)
            goto nomem;
    }
    if (json_object_set (cpy, "execution", execution) < 0)
        goto nomem;
    json_decref (execution);
    json_decref (props);
    return cpy;
nomem:
    json_decref (cpy);
    json_decref (execution);
    json_decref (props);
    errno = ENOMEM;  // set last: the decrefs above may clobber errno
    return NULL;
}

/* Build an rlist from 'R' so hosts can be resolved to ranks and properties
 * applied to it. Returns NULL on error with 'errp' set.
 */
static struct rlist *R_to_rlist (json_t *R, flux_error_t *errp)
{
    struct rlist *rl;

    if (!(rl = rlist_from_json (R, NULL))) {
        errprintf (errp, "queue membership: error parsing R");
        errno = EINVAL;
        return NULL;
    }
    return rl;
}

/* Splice the properties of decorated rlist 'rl' back into 'R', in place.
 *
 * Only execution.properties is replaced, so R's other fields (scheduling,
 * etc.) are preserved. The key is removed when no property remains, so an R
 * whose last property was removed looks the same as one that never had any.
 *
 * N.B. rlist_properties_encode() is used rather than rlist_to_R() because
 * it walks only the property hash. rlist_to_R() would sort and compress
 * every node and build a nodelist, all of it discarded here, and this runs
 * on the module load path via queue_membership_apply().
 *
 * Called only after every property has been applied successfully, so a
 * failure partway through leaves R untouched.
 */
static int splice_properties (json_t *R, struct rlist *rl, flux_error_t *errp)
{
    char *s;
    json_t *newprop = NULL;
    int rc = -1;

    if (!(s = rlist_properties_encode (rl))) {
        errprintf (errp,
                   "queue membership: rlist_properties_encode: %s",
                   strerror (errno));
        return -1;
    }
    if (!(newprop = json_loads (s, 0, NULL))) {
        errprintf (errp, "queue membership: error decoding R properties");
        errno = ENOMEM;
        goto done;
    }
    if (json_object_size (newprop) == 0) {
        if (jpath_del (R, "execution.properties") < 0) {
            errprintf (errp, "queue membership: error clearing R properties");
            goto done;
        }
    }
    else if (jpath_set (R, "execution.properties", newprop) < 0) {
        errprintf (errp, "queue membership: error updating R properties");
        errno = ENOMEM;
        goto done;
    }
    rc = 0;
done:
    ERRNO_SAFE_WRAP (json_decref, newprop);
    ERRNO_SAFE_WRAP (free, s);
    return rc;
}

int queue_membership_apply (json_t *R,
                            const flux_conf_t *conf,
                            flux_error_t *errp)
{
    json_t *queues = NULL;
    const char *name;
    json_t *entry;
    struct rlist *rl = NULL;
    int rc = -1;

    if (flux_conf_unpack (conf, NULL, "{s?o}", "queues", &queues) < 0)
        queues = NULL;
    /* Nothing to derive without a 'hosts' queue, which is the common case.
     * Check before building the rlist below, which is O(ranks).
     */
    if (!queue_membership_configured (queues))
        return 0;

    if (!(rl = R_to_rlist (R, errp)))
        goto done;
    json_object_foreach (queues, name, entry) {
        const char *hosts;
        char *ranks;
        char prop[QUEUE_PROPERTY_MAX];

        /* Only queues that declare 'hosts' get a membership property. A
         * virtual queue must not set 'hosts' (enforced by conf_policy), so a
         * 'hosts' queue always uses its own name for the property.
         */
        if (json_unpack (entry, "{s:s}", "hosts", &hosts) < 0)
            continue;
        if (!(ranks = hosts_to_idset_string (rl, name, hosts, false, errp)))
            goto done;
        (void)snprintf (prop, sizeof (prop), "+%s", name);
        if (rlist_add_property (rl, errp, prop, ranks) < 0) {
            ERRNO_SAFE_WRAP (free, ranks);
            goto done;
        }
        free (ranks);
    }
    if (splice_properties (R, rl, errp) < 0)
        goto done;
    rc = 0;
done:
    rlist_destroy (rl);
    return rc;
}

/* Resolve a runtime queue's recorded targets to an idset string of
 * ranks.
 *
 * Both forms were recorded at add time: 'targets' is an idset of ranks
 * and 'nodelist' the equivalent hostlist.  The rank is used as given,
 * and corrected from the hostlist only where the host now at that rank
 * differs, so that a restart which renumbers ranks still selects the
 * same nodes.  This mirrors add_target() in the resource module's
 * drain.c, and deliberately keeps the rank when hostnames are ambiguous
 * (every rank sharing one hostname, as on a test instance) rather than
 * collapsing every entry onto the first rank with that name.
 */
static char *runtime_targets_resolve (struct rlist *rl,
                                      const char *name,
                                      const char *targets,
                                      const char *nodelist,
                                      flux_t *h,
                                      flux_error_t *errp)
{
    struct idset *ids = NULL;
    struct idset *out = NULL;
    struct hostlist *hl = NULL;
    unsigned int rank;
    int index = 0;
    char *s = NULL;

    if (!nodelist
        || strlen (nodelist) == 0
        || !(ids = idset_decode (targets)))
        return hosts_to_idset_string (rl, name, targets, true, errp);
    if (!(hl = hostlist_decode (nodelist))
        || !(out = idset_create (0, IDSET_FLAG_AUTOGROW))) {
        errprintf (errp, "queue %s: error decoding recorded targets", name);
        goto done;
    }
    rank = idset_first (ids);
    while (rank != IDSET_INVALID_ID) {
        const char *host = hostlist_nth (hl, index++);
        const char *now = flux_get_hostbyrank (h, rank);
        unsigned int target = rank;
        int nrank;

        /* N.B. 'rank' must not be reassigned: it drives the iteration.
         */
        if (host && now && !streq (host, now)) {
            if ((nrank = flux_get_rankbyhost (h, host)) >= 0)
                target = nrank;
        }
        if (idset_set (out, target) < 0) {
            errprintf (errp, "queue %s: idset_set: %s", name, strerror (errno));
            goto done;
        }
        rank = idset_next (ids, rank);
    }
    if (idset_count (out) == 0) {
        errprintf (errp, "queue %s: targets resolve to an empty set", name);
        errno = EINVAL;
        goto done;
    }
    if (!(s = idset_encode (out, IDSET_FLAG_RANGE)))
        errprintf (errp, "queue %s: idset_encode: %s", name, strerror (errno));
done:
    idset_destroy (ids);
    idset_destroy (out);
    hostlist_destroy (hl);
    return s;
}

/* Apply the runtime queues described by 'runtime' to 'R', in place, and
 * collect the names of the started ones in 'exclusive'.
 *
 * Unlike queue_membership_apply(), a bad entry is not fatal: the key is
 * state the job-manager wrote earlier, and the configuration may have
 * changed under it since.  A conflicting or unresolvable entry is logged
 * and skipped, so one stale runtime queue cannot keep the module from
 * serving R.  The job-manager drops such an entry from the key on its
 * next save.
 */
int queue_membership_apply_runtime (json_t *R,
                                    json_t *runtime,
                                    json_t *exclusive,
                                    flux_t *h)
{
    json_t *queues;
    json_t *props;
    const char *name;
    json_t *entry;
    struct rlist *rl = NULL;
    int version;
    int rc = -1;

    if (json_unpack (runtime,
                     "{s:i s:o}",
                     "version", &version,
                     "queues", &queues) < 0
        || !json_is_object (queues)) {
        flux_log (h, LOG_ERR, "queues.runtime: malformed key, ignoring");
        return 0;
    }
    if (version != 1) {
        flux_log (h,
                  LOG_ERR,
                  "queues.runtime: unknown version %d, ignoring",
                  version);
        return 0;
    }
    if (json_object_size (queues) == 0)
        return 0;

    props = jpath_get (R, "execution.properties");
    if (!(rl = R_to_rlist (R, NULL)))
        return -1;
    json_object_foreach (queues, name, entry) {
        const char *hosts;
        const char *nodelist = NULL;
        char *ranks;
        char prop[QUEUE_PROPERTY_MAX];
        int started = 0;
        flux_error_t error;

        if (json_unpack (entry,
                         "{s:s s?s s?b}",
                         "targets", &hosts,
                         "nodelist", &nodelist,
                         "started", &started) < 0) {
            flux_log (h,
                      LOG_ERR,
                      "queues.runtime: queue %s: malformed entry, ignored",
                      name);
            continue;
        }
        (void)snprintf (prop, sizeof (prop), "+%s", name);
        /* A property already in R belongs to a configured queue of the
         * same name, which wins: the configuration is the authority.
         */
        if (props && json_object_get (props, prop)) {
            flux_log (h,
                      LOG_ERR,
                      "queues.runtime: queue %s conflicts with a configured"
                      " queue, ignored",
                      name);
            continue;
        }
        if (!(ranks = runtime_targets_resolve (rl,
                                               name,
                                               hosts,
                                               nodelist,
                                               h,
                                               &error))) {
            flux_log (h,
                      LOG_ERR,
                      "queues.runtime: queue %s: %s, ignored",
                      name,
                      error.text);
            continue;
        }
        if (rlist_add_property (rl, &error, prop, ranks) < 0) {
            flux_log (h,
                      LOG_ERR,
                      "queues.runtime: queue %s: %s, ignored",
                      name,
                      error.text);
            free (ranks);
            continue;
        }
        if (started && json_array_append_new (exclusive,
                                              json_string (prop)) < 0) {
            flux_log (h, LOG_ERR, "queues.runtime: out of memory");
            free (ranks);
            goto done;
        }
        flux_log (h,
                  LOG_INFO,
                  "queues.runtime: queue %s: %s on ranks %s%s",
                  name,
                  prop,
                  ranks,
                  started ? " (started, exclusive)" : "");
        free (ranks);
    }
    if (splice_properties (R, rl, NULL) < 0)
        goto done;
    rc = 0;
done:
    rlist_destroy (rl);
    return rc;
}

/* rlist_add_property() and rlist_remove_property() have this footprint.
 */
typedef int (*rlist_property_f)(struct rlist *rl,
                                flux_error_t *errp,
                                const char *name,
                                const char *targets);

/* Apply each property of 'properties' to 'rl' with 'fun', which is either
 * rlist_add_property() or rlist_remove_property(). Values must be idset
 * strings; the caller resolves any hostlist beforehand.
 */
static int apply_properties (struct rlist *rl,
                             json_t *properties,
                             rlist_property_f fun,
                             flux_error_t *errp)
{
    const char *name;
    json_t *val;

    if (!properties)
        return 0;
    if (!json_is_object (properties)) {
        errprintf (errp, "properties must be an object");
        errno = EINVAL;
        return -1;
    }
    json_object_foreach (properties, name, val) {
        const char *targets;

        if (!(targets = json_string_value (val))) {
            errprintf (errp, "property %s: targets must be a string", name);
            errno = EINVAL;
            return -1;
        }
        if (fun (rl, errp, name, targets) < 0)
            return -1;
    }
    return 0;
}

int queue_membership_update (json_t *R,
                             json_t *add,
                             json_t *remove,
                             flux_error_t *errp)
{
    struct rlist *rl;
    int rc = -1;

    if (!(rl = R_to_rlist (R, errp)))
        return -1;
    /* Remove before add so that moving a rank between two queues in one
     * update -- remove "+A", add "+B" -- cannot be ordered such that the
     * add is undone by the remove.
     */
    if (apply_properties (rl, remove, rlist_remove_property, errp) < 0
        || apply_properties (rl, add, rlist_add_property, errp) < 0)
        goto done;
    if (splice_properties (R, rl, errp) < 0)
        goto done;
    rc = 0;
done:
    rlist_destroy (rl);
    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
