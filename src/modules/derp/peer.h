/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _DERP_PEER_H
#define _DERP_PEER_H

#include <jansson.h>
#include <flux/core.h>

#include "src/common/libczmqcontainers/czmq_containers.h"

#include "hello.h"

struct peer {
    uint32_t rank;
    struct idset *idset;
    const flux_msg_t *msg;

    unsigned int connected:1;
    zlistx_t *pending;
};

struct peers {
    int child_count;
    struct idset *idset;
    struct peer *children;
};

struct peers * peers_create (json_t *topology);

void peers_destroy (struct peers *peers);

struct peer *peer_lookup (struct peers *peers, int rank);

struct peer *peer_connect (struct peers *peers, const flux_msg_t *msg);

void peer_disconnect (struct peers *peers, const flux_msg_t *msg);

int peer_forward_response (flux_t *h,
                           struct peers *peers,
                           struct hello_response *hresp);

int peer_process_pending (flux_t *h, struct peer *child);

#endif /* !_DERP_PEER_H */
