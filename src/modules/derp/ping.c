/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* ping: test distributed hierarchical message passing via a ping service.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <flux/core.h>
#include <flux/idset.h>

#include "derp.h"

struct ping {
    struct derp_ctx *ctx;
    flux_msg_handler_t **handlers;

    const flux_msg_t *request;
    struct idset *idset;
    struct idset *reply_idset;
};


static void ping_clear (struct ping *ping)
{
    idset_destroy (ping->idset);
    idset_destroy (ping->reply_idset);
    flux_msg_decref (ping->request);

    ping->idset = NULL;
    ping->reply_idset = NULL;
    ping->request = NULL;
}

static int ping_respond (struct ping *ping)
{
    int rc = -1;
    char *ranks;
    struct derp_ctx *ctx = ping->ctx;

    if (!(ranks = idset_encode (ping->idset, IDSET_FLAG_RANGE))) {
        flux_log_error (ctx->h, "ping_respond: idset_encode");
        return -1;
    }
    if (ping->request) {
        flux_log (ctx->h,
                  LOG_DEBUG,
                  "ping: all replies for %s received.",
                  ranks);
        if (flux_respond_pack (ctx->h,
                               ping->request,
                               "{s:s}",
                               "ranks", ranks) < 0) {
            flux_log_error (ctx->h, "ping_respond: flux_respond");
            goto error;
        }
    }
    else {
        flux_future_t *f;

        flux_log (ctx->h,
                  LOG_DEBUG,
                  "ping: %s complete. notifying upstream",
                  ranks);
        if (!(f = flux_rpc_pack (ctx->h,
                                 "derp.notify",
                                 FLUX_NODEID_UPSTREAM,
                                 FLUX_RPC_NORESPONSE,
                                 "{s:s s:{s:s}}",
                                 "type", "ping-reply",
                                 "data",
                                 "ranks", ranks))) {
            flux_log_error (ctx->h, "ping_try_response: flux_rpc");
            goto error;
        }
        flux_future_destroy (f);
    }
    rc = 0;
error:
    free (ranks);
    return rc;
}

/*  If all downstream replies have been recvd, forward them upstream
 *   or repond to original ping request.
 */
static int ping_try_response (struct ping *ping)
{
    if (idset_equal (ping->idset, ping->reply_idset)) {
        if (ping_respond (ping) < 0)
            return -1;
        ping_clear (ping);
    }
    return 0;
}

static int ping_handler (void *arg,
                         const char *name,
                         const char *idset,
                         json_t *data)
{
    struct ping *ping = arg;

    flux_log (ping->ctx->h,
              LOG_DEBUG,
              "ping_handler: idset=%s",
              idset);

    if (!(ping->idset = idset_decode (idset))
        || !(ping->reply_idset = idset_create (0, IDSET_FLAG_AUTOGROW)))
        return -1;

    /*  Set current rank in idset, if this rank is a target
     */
    if (idset_test (ping->idset, ping->ctx->rank)
        && idset_set (ping->reply_idset, ping->ctx->rank) < 0)
        return -1;

    /*  Check if all expected replies have been received and reply
     */
    return ping_try_response (ping);
}

/* return true if a is a subset of b */
static bool is_subset_of (const struct idset *a, const struct idset *b)
{
    struct idset *ids;
    int count;

    if (!(ids = idset_difference (a, b)))
        return false;
    count = idset_count (ids);
    idset_destroy (ids);
    if (count > 0)
        return false;
    return true;
}

static void ping_request (flux_t *h,
                          flux_msg_handler_t *mh,
                          const flux_msg_t *msg,
                          void *arg)
{
    struct ping *ping = arg;
    const char *ranks;
    struct idset *idset;
    bool is_subset = false;
    json_t *data;

    if (!ping || !ping->ctx) {
        errno = EINVAL;
        goto error;
    }

    /*  Only allow a single ping request to be active for now
     */
    if (ping->request) {
        errno = EAGAIN;
        goto error;
    }

    if (flux_request_unpack (msg,
                             NULL,
                             "{s:s s:o}",
                             "ranks", &ranks,
                             "data", &data) < 0)
        goto error;

    /*  Ensure ranks idset can be decoded.
     *  Then remove the current rank from the idset and ensure the
     *   requested ranks is a subset of peer ranks before proceeding.
     */
    if (!(idset = idset_decode (ranks))
        || idset_clear (idset, ping->ctx->rank))
        goto error;
    is_subset = is_subset_of (idset, ping->ctx->peers->idset);
    idset_destroy (idset);
    if (!is_subset) {
        errno = ENOENT;
        goto error;
    }

    flux_log (h, LOG_DEBUG, "ping: starting ping to ranks %s", ranks);

    ping->request = flux_msg_incref (msg);

    /*  Note: derp_forward() calls local handler on rank 0
     */
    if (derp_forward (ping->ctx, "ping", ranks, "O", data) < 0) {
        flux_log_error (h, "ping: forward");
        goto error;
    }

    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "ping: flux_respond_error");
}

static void ping_reply (flux_t *h,
                        const flux_msg_t *msg,
                        json_t *data,
                        void *arg)
{
    int rc;
    char *s;
    struct ping *ping = arg;
    const char *ranks = NULL;
    struct idset *idset = NULL;

    /*  This is a ping reply from indicated 'ranks'
     *  It comes as a request so we have to unpack it as such:
     */
    if (json_unpack (data, "{s:s}", "ranks", &ranks) < 0
        || !(idset = idset_decode (ranks))) {
        flux_log_error (h,
                        "ping_response: failed to get ping resp ranks: %s",
                        flux_msg_last_error (msg));
        return;
    }
    rc = idset_add (ping->reply_idset, idset);
    idset_destroy (idset);
    if (rc < 0)
        flux_log_error (h, "ping_reply: idset_add (%s, %s)",
                        idset_encode (ping->reply_idset, IDSET_FLAG_RANGE),
                        ranks);

    s = idset_encode (ping->reply_idset, IDSET_FLAG_RANGE);
    flux_log (h,
              LOG_DEBUG,
              "ping_reply from %s: (total=%s)",
              ranks,
              s);
    free (s);
    /*  ping_reply logs appropriate error */
    (void) ping_try_response (ping);
    return;
}


static const struct flux_msg_handler_spec htab[] = {
    {
        .typemask = FLUX_MSGTYPE_REQUEST,
        .topic_glob = "derp.ping",
        .cb = ping_request,
        .rolemask = 0
    },
    FLUX_MSGHANDLER_TABLE_END
};

static void ping_ctx_destroy (struct ping *ping)
{
    if (ping) {
        int saved_errno = errno;
        flux_msg_handler_delvec (ping->handlers);
        ping_clear (ping);
        free (ping);
        errno = saved_errno;
    }
}

static struct ping * ping_ctx_create (struct derp_ctx *ctx)
{
    struct ping *ping;

    if (!(ping = calloc (1, sizeof (*ping))))
        return NULL;
    ping->ctx = ctx;
    if (flux_msg_handler_addvec (ctx->h, htab, ping, &ping->handlers) < 0)
        goto error;
    return ping;
error:
    ping_ctx_destroy (ping);
    return NULL;
}

int ping_init (struct derp_ctx *ctx)
{
    struct ping *ping = ping_ctx_create (ctx);
    if (!ping)
        return -1;
    derp_register_action (ctx,
                          "ping",
                          ping_handler,
                          (flux_free_f) ping_ctx_destroy,
                          ping);
    derp_register_notify (ctx,
                          "ping-reply",
                          ping_reply,
                          ping);
    return 0;
}
