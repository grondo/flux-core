/************************************************************\
 * Copyright 2022 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _DERP_HELLO_H
#define _DERP_HELLO_H

#include <stdarg.h>

#include <jansson.h>
#include <flux/core.h>
#include <flux/idset.h>

struct hello_responder;

struct hello_response {
    int refcount;
    char *type;          /* type of response */
    char *ids;           /* encoded idset string */
    struct idset *idset; /* Set of all receivers */
    json_t *data;        /* data part of payload */
};

struct hello_responder * hello_responder_create ();
void hello_responder_destroy (struct hello_responder *hr);

struct hello_response *hello_response_vpack (const char *type,
                                             const struct idset *idset,
                                             const char *idset_str,
                                             const char *fmt,
                                             va_list ap);

struct hello_response *hello_response_pack (const char *type,
                                            struct idset *idset,
                                            const char *idset_str,
                                            const char *fmt,
                                            ...);

int hello_responder_push (struct hello_responder *hr,
                          const char *type,
                          flux_jobid_t id,
                          uint32_t userid,
                          struct idset *ranks);

struct hello_response *hello_responder_pop (struct hello_responder *hr);

int hello_responder_count (struct hello_responder *hr);

struct hello_response *hello_response_incref (struct hello_response *hresp);
void hello_response_decref (struct hello_response *hreas);

void hello_response_destroy (struct hello_response *hresp);

#endif /* !_DERP_HELLO_H */
