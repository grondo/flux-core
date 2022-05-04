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
#include <jansson.h>
#include <flux/core.h>

#include "src/common/libidset/idset.h"
#include "src/common/libutil/errno_safe.h"

#include "peer.h"
#include "hello.h"

/* Recursive function to walk 'topology', adding all subtree ranks to 'ids'.
 * Returns 0 on success, -1 on failure (errno is not set).
 *
 * Note: Lifted directly from src/broker/groups.c
 */
static int add_subtree_ids (struct idset *ids, json_t *topology)
{
    int rank;
    json_t *a;
    size_t index;
    json_t *entry;

    if (json_unpack (topology, "{s:i s:o}", "rank", &rank, "children", &a) < 0
        || idset_set (ids, rank) < 0)
        return -1;
    json_array_foreach (a, index, entry) {
        if (add_subtree_ids (ids, entry) < 0)
            return -1;
    }
    return 0;
}

/*  Note: This implementation assumes subtree_rank is one of the direct
 *   children of the first level of `topology`. Therefore, the entire
 *   topology does not need to be traversed, only the first level.
 */
static json_t *get_subtree_topology (json_t *topology, int subtree_rank)
{
    int rank;
    json_t *a;
    size_t index;
    json_t *entry;

    if (json_unpack (topology, "{s:i s:o}", "rank", &rank, "children", &a) < 0)
        return NULL;
    if (rank == subtree_rank)
        return topology;
    json_array_foreach (a, index, entry) {
        if (json_unpack (entry, "{s:i}", "rank", &rank) < 0)
            return NULL;
        if (rank == subtree_rank)
            return entry;
    }
    return NULL;
}

static struct idset * peer_subtree_idset (uint32_t rank, json_t *topo)
{
    struct idset *ids = NULL;
    json_t *topology;

    if (!(topology = get_subtree_topology (topo, rank))
        || !(ids = idset_create (0, IDSET_FLAG_AUTOGROW))
        || add_subtree_ids (ids, topology) < 0) {
        idset_destroy (ids);
        return NULL;
    }
    return ids;
}

/*  zlistx destructor prototype
 */
static void response_destructor (void **item)
{
    if (item) {
        struct hello_response *hresp = *item;
        hello_response_destroy (hresp);
        *item = NULL;
    }
}

static int child_init (struct peer *child, int rank, json_t *topo)
{
    child->rank = rank;
    if (!(child->idset = peer_subtree_idset (rank, topo)))
        return -1;
    if (!(child->pending = zlistx_new ()))
        return -1;
    zlistx_set_destructor (child->pending, response_destructor);
    return 0;
}

static void child_free (struct peer *child)
{
    if (child) {
        zlistx_destroy (&child->pending);
        idset_destroy (child->idset);
        flux_msg_decref (child->msg);
    }
}

void peers_destroy (struct peers *peers)
{
    if (peers) {
        if (peers->children) {
            for (int i = 0; i < peers->child_count; i++)
                child_free (&peers->children[i]);
        }
        idset_destroy (peers->idset);
        free (peers->children);
        free (peers);
    }
}

struct peers *peers_create (json_t *topology)
{
    json_t *children;
    json_t *entry;
    size_t index;
    struct peers *peers;

    if (json_unpack (topology, "{s:o}", "children", &children) < 0
        || !json_is_array (children)) {
        errno = EPROTO;
        return NULL;
    }

    if (!(peers = calloc (1, sizeof (*peers))))
        return NULL;

    peers->child_count = json_array_size (children);
    if (!(peers->idset = idset_create (0, IDSET_FLAG_AUTOGROW))
        || !(peers->children = calloc (peers->child_count,
                                       sizeof (struct peer))))
        goto error;

    json_array_foreach (children, index, entry) {
        int rank;
        struct peer *child = &peers->children[index];

        if (json_unpack (entry, "{s:i}", "rank", &rank) < 0) {
            errno = EPROTO;
            goto error;
        }
        if (child_init (child, rank, topology) < 0)
            goto error;
        if (idset_add (peers->idset, child->idset) < 0)
            goto error;
    }
    return peers;
error:
    peers_destroy (peers);
    return NULL;
}

struct peer *peer_lookup (struct peers *peers, int rank)
{
    for (int i = 0; i < peers->child_count; i++) {
        if (peers->children[i].rank == rank)
            return &peers->children[i];
    }
    errno = ENOENT;
    return NULL;
}

struct peer *peer_connect (struct peers *peers,
                                     const flux_msg_t *msg)
{
    struct peer *child;
    int rank;

    if (flux_request_unpack (msg, NULL, "{s:i}", "rank", &rank) < 0) {
        errno = EPROTO;
        return NULL;
    }
    if (!(child = peer_lookup (peers, rank)))
        return NULL;
    child->msg = flux_msg_incref (msg);
    child->connected = 1;
    return child;
}

/*  peer disconnect handler */
void peer_disconnect (struct peers *peers,
                           const flux_msg_t *msg)
{
    for (int i = 0; i < peers->child_count; i++) {
        struct peer *child = &peers->children[i];
        if (flux_msg_route_match_first (child->msg, msg)) {
            flux_msg_decref (child->msg);
            child->msg = NULL;
            child->connected = 0;
        }
    }
}

static int peer_hello_respond (flux_t *h,
                               struct peer *child,
                               struct hello_response *hresp)
{
    int rc = -1;
    struct idset *intersect = NULL;
    char *ids = NULL;

    /*  Only target the intersection of the child ranks and the
     *   the response target ranks.
     */
    if (!(intersect = idset_intersect (hresp->idset, child->idset))
        || !(ids = idset_encode (intersect, IDSET_FLAG_RANGE)))
        goto out;

    rc = flux_respond_pack (h,
                            child->msg,
                            "{s:s s:s s:O}",
                            "idset", ids,
                            "type", hresp->type,
                            "data", hresp->data);
out:
    idset_destroy (intersect);
    ERRNO_SAFE_WRAP (free, ids);
    return rc;
}

int peer_process_pending (flux_t *h, struct peer *child)
{
    int rc = 0;
    if (child && child->connected) {
        struct hello_response *hresp = zlistx_first (child->pending);
        while (hresp) {
            if (peer_hello_respond (h, child, hresp) < 0) {
                flux_log_error (h, "peer_respond");
                return 0;
            }
            zlistx_detach_cur (child->pending);
            hello_response_decref (hresp);
            hresp = zlistx_next (child->pending);
        }
    }
    return rc;
}


int peer_forward_response (flux_t *h,
                           struct peers *peers,
                           struct hello_response *hresp)
{
    int rc = 0;
    char *ids;

    if (!(ids = idset_encode (hresp->idset, IDSET_FLAG_RANGE)))
        return -1;

    for (int i = 0; i < peers->child_count; i++) {
        struct peer *child = &peers->children[i];
        if (idset_has_intersection (child->idset, hresp->idset)) {
            if (child->connected) {
                if (peer_hello_respond (h, child, hresp) < 0)
                    rc = -1;
            }
            else {
                if (zlistx_add_end (child->pending,
                                    hello_response_incref (hresp)) < 0)
                    rc = -1;
            }
        }
    }
    free (ids);
    return rc;
}
