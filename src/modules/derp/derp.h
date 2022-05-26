/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _DERP_H
#define _DERP_H

#include <jansson.h>
#include <flux/core.h>
#include "src/common/libczmqcontainers/czmq_containers.h"

#include "peer.h"
#include "hello.h"


struct derp_ctx {
    flux_t *h;
    flux_msg_handler_t **handlers;

    uint32_t rank;
    json_t *topology;
    flux_future_t *hello_f;

    struct peers *peers;
    struct hello_responder *hr;

    zhashx_t *jobs;

    zhashx_t *actions;
    zhashx_t *notifications;
};

typedef int (*derp_action_f) (void *arg,
                              const char *name,
                              const char *idset,
                              json_t *data);

typedef void (*derp_notify_f) (flux_t *h,
                               const flux_msg_t *msg,
                               json_t *data,
                               void *arg);


/*  Register action for handling messages of 'type'
 */
int derp_register_action (struct derp_ctx *ctx,
                          const char *type,
                          derp_action_f fn,
                          flux_free_f free_fn,
                          void *arg);

int derp_register_notify (struct derp_ctx *ctx,
                          const char *type,
                          derp_notify_f fn,
                          void *arg);

/*  Forward a message of 'type' addressed to 'ranks' downstream
 *   (via hello protocol response)
 *   If on rank 0, local action handler is also called.
 */
int derp_forward (struct derp_ctx *ctx,
                  const char *type,
                  const char *ranks,
                  const char *fmt,
                  ...);


#endif /* !_DERP_PEER_H */
