/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _BROKER_OVERLAY_H
#define _BROKER_OVERLAY_H

#include "attr.h"

enum {
    KEEPALIVE_STATUS_NORMAL = 0,
    KEEPALIVE_STATUS_DISCONNECT = 1,
};

struct overlay;

typedef void (*overlay_sock_cb_f)(struct overlay *ov, void *arg);
typedef int (*overlay_init_cb_f)(struct overlay *ov, void *arg);
typedef void (*overlay_monitor_cb_f)(struct overlay *ov, void *arg);

struct overlay *overlay_create (flux_t *h);
void overlay_destroy (struct overlay *ov);

/* Set a callback triggered during overlay_init()
 */
void overlay_set_init_callback (struct overlay *ov,
                                overlay_init_cb_f cb,
                                void *arg);

/* These need to be called before connect/bind.
 */
int overlay_init (struct overlay *ov,
                  uint32_t size,
                  uint32_t rank,
                  int tbon_k);
void overlay_set_idle_warning (struct overlay *ov, int heartbeats);


/* CURVE key management
 * If downstream peers, call overlay_authorize() with public key of each peer.
 */
int overlay_cert_load (struct overlay *ov, const char *path);
const char *overlay_cert_pubkey (struct overlay *ov);
const char *overlay_cert_name (struct overlay *ov);
int overlay_authorize (struct overlay *ov,
                       const char *name,
                       const char *pubkey);

/* Accessors
 */
uint32_t overlay_get_rank (struct overlay *ov);
uint32_t overlay_get_size (struct overlay *ov);
int overlay_get_child_peer_count (struct overlay *ov);

/* All ranks but rank 0 connect to a parent to form the main TBON.
 */
int overlay_set_parent_uri (struct overlay *ov, const char *uri);
int overlay_set_parent_pubkey (struct overlay *ov, const char *pubkey);
const char *overlay_get_parent_uri (struct overlay *ov);
void overlay_set_parent_cb (struct overlay *ov,
                            overlay_sock_cb_f cb,
                            void *arg);
int overlay_sendmsg_parent (struct overlay *ov, const flux_msg_t *msg);
flux_msg_t *overlay_recvmsg_parent (struct overlay *ov);

/* The child is where other ranks connect to send requests.
 * This is the ROUTER side of parent sockets described above.
 */
int overlay_bind (struct overlay *ov, const char *uri);
const char *overlay_get_bind_uri (struct overlay *ov);
void overlay_set_child_cb (struct overlay *ov, overlay_sock_cb_f cb, void *arg);
int overlay_sendmsg_child (struct overlay *ov, const flux_msg_t *msg);
flux_msg_t *overlay_recvmsg_child (struct overlay *ov);

/* We can "multicast" events to all child peers using mcast_child().
 * It walks the 'children' hash, finding peers and routeing them a copy of msg.
 */
void overlay_mcast_child (struct overlay *ov, const flux_msg_t *msg);

/* Call when message is received from child 'uuid'.
 * If message was a keepalive, update 'status', otherwise set to zero.
 */
void overlay_keepalive_child (struct overlay *ov, const char *uuid, int status);

/* Register callback that will be called each time a child connects/disconnects.
 * Use overlay_get_child_peer_count() to access the actual count.
 */
void overlay_set_monitor_cb (struct overlay *ov,
                             overlay_monitor_cb_f cb,
                             void *arg);

/* Establish communication with parent.
 */
int overlay_connect (struct overlay *ov);

/* Add attributes to 'attrs' to reveal information about the overlay network.
 * Active attrs:
 *   tbon.parent-endpoint
 * Passive attrs:
 *   rank
 *   size
 *   tbon.arity
 *   tbon.level
 *   tbon.maxlevel
 *   tbon.descendants
 * Returns 0 on success, -1 on error.
 */
int overlay_register_attrs (struct overlay *overlay, attr_t *attrs);

#endif /* !_BROKER_OVERLAY_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
