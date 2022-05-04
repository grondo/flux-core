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
#include "ccan/str/str.h"

#include "derp.h"
#include "job.h"
#include "peer.h"
#include "hello.h"

struct derp_action {
    derp_action_f fn;
    flux_free_f   destroy;
    void *        arg;
};

static void derp_action_destroy (struct derp_action *action)
{
    if (action) {
        int saved_errno = errno;
        if (action->destroy)
            (*action->destroy) (action->arg);
        free (action);
        errno = saved_errno;
    }
}

static void derp_action_destructor (void **item)
{
    if (item) {
        derp_action_destroy (*item);
        *item = NULL;
    }
}

static struct derp_action * derp_action_create (derp_action_f fn,
                                                flux_free_f destroy,
                                                void *arg)
{
    struct derp_action *action;
    if (!(action = calloc (1, sizeof (*action))))
        return NULL;
    action->fn = fn;
    action->destroy = destroy;
    action->arg = arg;
    return action;
}

static void derp_ctx_destroy (struct derp_ctx *ctx)
{
    if (ctx) {
        peers_destroy (ctx->peers);
        hello_responder_destroy (ctx->hr);
        json_decref (ctx->topology);
        zhashx_destroy (&ctx->jobs);
        zhashx_destroy (&ctx->actions);
        flux_future_destroy (ctx->hello_f);
        flux_msg_handler_delvec (ctx->handlers);
        free (ctx);
    }
}

static json_t *get_topology (flux_t *h, int rank)
{
    json_t *topology = NULL;
    flux_future_t *f;

    if (!(f = flux_rpc_pack (h,
                             "overlay.topology",
                             FLUX_NODEID_ANY,
                             0,
                             "{s:i}",
                             "rank", rank))
        || flux_rpc_get_unpack (f, "O", &topology) < 0) {
        flux_log_error (h, "overlay.topology");
        return NULL;
    }
    flux_future_destroy (f);
    return topology;
}

static struct derp_ctx *derp_ctx_create (flux_t *h)
{
    struct derp_ctx *ctx;
    uint32_t rank;
    json_t *topology;

    if (flux_get_rank (h, &rank) < 0
        || !(topology = get_topology (h, rank))
        || !(ctx = calloc (1, sizeof (*ctx))))
        return NULL;

    ctx->h = h;
    ctx->rank = rank;
    ctx->topology = topology;

    if (!(ctx->peers = peers_create (topology))
        || !(ctx->hr = hello_responder_create ())
        || !(ctx->jobs = derp_job_hash_create ())
        || !(ctx->actions = zhashx_new ()))
        goto error;

    zhashx_set_destructor (ctx->actions, derp_action_destructor);

    return ctx;
error:
    derp_ctx_destroy (ctx);
    return NULL;
}

int derp_forward (struct derp_ctx *ctx,
                  const char *type,
                  const char *idset,
                  const char *fmt,
                  ...)
{
    int rc;
    va_list ap;
    struct hello_response *hresp;
    struct derp_action *action;

    va_start (ap, fmt);
    hresp = hello_response_vpack (type, NULL, idset, fmt, ap);
    va_end (ap);
    if (!hresp)
        return -1;

    /*  Forward response down to matching peers */
    rc = peer_forward_response (ctx->h, ctx->peers, hresp);

    /*  Handle response locally on rank 0 only
     *
     *  This allows rank 0 to "forward" a response to itself and
     *   all other ranks, whereas internal ranks will have naturally
     *   already handled this response upon receipt.
     */
    if (rc >= 0
        && ctx->rank == 0
        && (action = zhashx_lookup (ctx->actions, type)))
        rc = (*action->fn) (action->arg, type, idset, hresp->data);

    hello_response_destroy (hresp);

    return rc;

}

int derp_register (struct derp_ctx *ctx,
                   const char *type,
                   derp_action_f fn,
                   flux_free_f free_fn,
                   void *arg)
{
    struct derp_action *action = derp_action_create (fn, free_fn, arg);
    if (!action)
        return -1;
    if (zhashx_insert (ctx->actions, type, action) < 0) {
        errno = EEXIST;
        derp_action_destroy (action);
        return -1;
    }
    return 0;
}

static int hello_response_handler (struct derp_ctx *ctx, flux_future_t *f)
{
    int rc = -1;
    const char *type;
    const char *idset;
    json_t *data;
    struct derp_action *action;

    if (flux_rpc_get_unpack (f,
                             "{s:s s:s s:o}",
                             "type", &type,
                             "idset", &idset,
                             "data", &data) < 0)
        goto out;

    /*  Forward to downstream peers if necessary
     */
    if (derp_forward (ctx, type, idset, "O", data) < 0)
        goto out;

    /*  Handle individual update types:
     */
    if (!(action = zhashx_lookup (ctx->actions, type))) {
        flux_log (ctx->h,
                  LOG_ERR,
                  "No handler for hello response type=%s. Ignoring.",
                  type);
        goto out;
    }
    rc = (*action->fn) (action->arg, type, idset, data);
out:
    flux_future_reset (f);
    return rc;
}

static void hello_continuation (flux_future_t *f, void *arg)
{
    struct derp_ctx *ctx = arg;
    if (hello_response_handler (ctx, f) < 0)
        flux_log_error (ctx->h, "hello_response_handler");
}

static int derp_hello (struct derp_ctx *ctx)
{
    flux_future_t *f;

    if (ctx->rank == 0)
        return 0;

    if (!(f = flux_rpc_pack (ctx->h,
                             "derp.hello",
                             FLUX_NODEID_UPSTREAM,
                             FLUX_RPC_STREAMING,
                             "{s:i}",
                             "rank", ctx->rank))) {
        flux_log_error (ctx->h, "sending derp.hello");
        return -1;
    }
    if (flux_future_then (f, -1., hello_continuation, ctx) < 0) {
        flux_log_error (ctx->h, "derp_hello: flux_future_then");
        goto error;
    }
    ctx->hello_f = f;
    return 0;
error:
    flux_future_destroy (f);
    return -1;
}

static void derp_hello_cb (flux_t *h,
                           flux_msg_handler_t *mh,
                           const flux_msg_t *msg,
                           void *arg)
{
    struct derp_ctx *ctx = arg;
    struct peer *peer;
    char *s;

    if (!(peer = peer_connect (ctx->peers, msg)))
        goto error;

    s = idset_encode (peer->idset, IDSET_FLAG_RANGE);
    flux_log (h,
              LOG_DEBUG,
              "connection from peer rank %u [subtree=%s]",
              peer->rank,
              s);

    /*  First response is synchronous:
     *  Only if there is currently any state to send.
     */
    #if 0
    if (flux_respond_pack (h,
                           msg,
                           "{s:s s:s s:{s:[]}}",
                           "type", "state-update",
                           "idset", s,
                           "data",
                           "jobs") < 0)
        flux_log_error (h, "derp.hello: flux_respond");

    #endif
    /* Then send any pending updates
     */
    peer_process_pending (h, peer);
    free (s);
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "derp.hello: flux_respond_error");
}

static void derp_disconnect_cb (flux_t *h,
                                flux_msg_handler_t *mh,
                                const flux_msg_t *msg,
                                void *arg)
{
    struct derp_ctx *ctx = arg;
    peer_disconnect (ctx->peers, msg);
}

static const struct flux_msg_handler_spec htab[] = {
    {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .topic_glob = "derp.hello",
        .cb = derp_hello_cb,
        .rolemask = 0
    },
    {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .topic_glob = "derp.disconnect",
        .cb = derp_disconnect_cb,
        .rolemask = 0
    },
    FLUX_MSGHANDLER_TABLE_END,
};

/*  List of external init functions:
 */
extern int ping_init (struct derp_ctx *ctx);
extern int exec_init (struct derp_ctx *ctx);

int mod_main (flux_t *h, int argc, char **argv)
{
    int rc = -1;
    struct derp_ctx *ctx = derp_ctx_create (h);
    if (!ctx)
        goto error;
    if (ping_init (ctx) < 0)
        goto error;
    if (exec_init (ctx) < 0)
        goto error;
    if (derp_hello (ctx) < 0)
        goto error;
    if (flux_msg_handler_addvec (h, htab, ctx, &ctx->handlers) < 0)
        goto error;
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0) {
        flux_log_error (h, "flux_reactor_run");
        goto error;
    }
    rc = 0;
error:
    derp_ctx_destroy (ctx);
    return rc;
}

MOD_NAME ("derp");
