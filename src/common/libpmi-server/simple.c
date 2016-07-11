/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms f the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <czmq.h>

#include "simple.h"

#include "src/common/libutil/oom.h"
#include "src/common/libutil/xzmalloc.h"

/* N.B. These values cannot be numeric expressions as they are incorporated
 * into sscanf conversion specifiers with preprocessor trick, hence these
 * definitions may seem backwards.
 */
#define KVS_KEY_MAX_SCAN  63
#define KVS_VAL_MAX_SCAN  511
#define KVS_NAME_MAX_SCAN 63

#define KVS_KEY_MAX     (KVS_KEY_MAX_SCAN+1)
#define KVS_VAL_MAX     (KVS_VAL_MAX_SCAN+1)
#define KVS_NAME_MAX    (KVS_NAME_MAX_SCAN+1)

#define MAX_PROTO_OVERHEAD  64

#define MAX_PROTO_LINE \
    (KVS_KEY_MAX + KVS_VAL_MAX + KVS_NAME_MAX + MAX_PROTO_OVERHEAD)

struct pmi_simple_server {
    void *arg;
    struct pmi_simple_ops ops;
    int appnum;
    char *kvsname;
    int universe_size;
    int local_procs;
    zlist_t *barrier;
    int debug;
};

struct pmi_simple_server *pmi_simple_server_create (struct pmi_simple_ops *ops,
                                                    int appnum,
                                                    int universe_size,
                                                    int local_procs,
                                                    const char *kvsname,
                                                    void *arg)
{
    struct pmi_simple_server *pmi = xzmalloc (sizeof (*pmi));
    const char *s;

    pmi->ops = *ops;
    pmi->arg = arg;
    pmi->appnum = appnum;
    pmi->kvsname = xstrdup (kvsname);
    pmi->universe_size = universe_size;
    pmi->local_procs = local_procs;
    if ((s = getenv ("PMI_DEBUG")))
        pmi->debug = strtoul (s, NULL, 10);
    if (!(pmi->barrier = zlist_new ()))
        oom ();
    return pmi;
}

void pmi_simple_server_destroy (struct pmi_simple_server *pmi)
{
    if (pmi) {
        if (pmi->kvsname)
            free (pmi->kvsname);
        zlist_destroy (&pmi->barrier);
        free (pmi);
    }
}

int pmi_simple_server_get_maxrequest (struct pmi_simple_server *pmi)
{
    return (MAX_PROTO_LINE);
}

static int barrier_enter (struct pmi_simple_server *pmi, void *client)
{
    if (zlist_append (pmi->barrier, client) < 0)
        oom ();
    return 0;
}

static int barrier_exit (struct pmi_simple_server *pmi, int rc)
{
    char resp[MAX_PROTO_LINE+1];
    void *client;
    int ret = 0;

    while ((client = zlist_pop (pmi->barrier))) {
        /* XXX the protocol doesn't allow an error to be returned
         * for the barrier operation, so we return "barrier_failed"
         * instead of "barrier_out", which should trigger a protocol error.
         * We throw our rc code in there without expectation that it's
         * going anywhere useful, unless client prints the unexpected
         * message it received.
         */
        if (rc != 0)
            snprintf (resp, sizeof (resp), "cmd=barrier_failed rc=%d\n", rc);
        else
            snprintf (resp, sizeof (resp), "cmd=barrier_out\n");
        if (pmi->debug)
            fprintf (stderr, "S: (client=%p) %s", client, resp);
        if (pmi->ops.response_send (client, resp) < 0)
            ret = -1;
    }
    return ret;
}

#define S_(x) #x
#define S(x) S_(x)

int pmi_simple_server_request (struct pmi_simple_server *pmi,
                               const char *buf, void *client)
{
    char key[KVS_KEY_MAX];
    char val[KVS_VAL_MAX];
    char name[KVS_NAME_MAX];
    char resp[MAX_PROTO_LINE+1];
    int send_response = 1;
    int rc = 0;

    if (pmi->debug)
        fprintf (stderr, "C: (client=%p) %s", client, buf);

    if (!strcmp (buf, "cmd=init pmi_version=1 pmi_subversion=1\n")) {
        snprintf (resp, sizeof (resp),
                  "cmd=response_to_init pmi_version=1 pmi_subversion=1 rc=0\n");
    } else if (!strcmp (buf, "cmd=get_maxes\n")) {
        snprintf (resp, sizeof (resp),
                  "cmd=maxes kvsname_max=%d keylen_max=%d vallen_max=%d\n",
                  KVS_NAME_MAX, KVS_KEY_MAX, KVS_VAL_MAX);
    } else if (!strcmp (buf, "cmd=get_appnum\n")) {
        snprintf (resp, sizeof (resp), "cmd=appnum appnum=%d\n", pmi->appnum);
    } else if (!strcmp (buf, "cmd=get_my_kvsname\n")) {
        snprintf (resp, sizeof (resp),
                  "cmd=my_kvsname kvsname=%s\n", pmi->kvsname);
    } else if (!strcmp (buf, "cmd=get_universe_size\n")) {
        snprintf (resp, sizeof (resp), "cmd=universe_size size=%d\n",
                  pmi->universe_size);
    } else if (sscanf (buf, "cmd=put"
                            " kvsname=%" S(KVS_NAME_MAX_SCAN) "s"
                            " key=%" S(KVS_KEY_MAX_SCAN) "s"
                            " value=%" S(KVS_VAL_MAX_SCAN) "s",
                            name, key, val) == 3) {
        int result = pmi->ops.kvs_put (pmi->arg, name, key, val);
        snprintf (resp, sizeof (resp), "cmd=put_result rc=%d msg=%s\n",
                  result, result == 0 ? "success" : "failure");
    } else if (sscanf (buf, "cmd=get"
                            " kvsname=%" S(KVS_NAME_MAX_SCAN) "s"
                            " key=%" S(KVS_KEY_MAX_SCAN) "s",
                            name, key) == 2) {
        int result = pmi->ops.kvs_get (pmi->arg, name, key, val, KVS_VAL_MAX);
        snprintf (resp, sizeof (resp),
                  "cmd=get_result rc=%d msg=%s value=%s\n", result,
                  result == 0 ? "success" : "failure",
                  result == 0 ? val : "");
    } else if (!strcmp (buf, "cmd=barrier_in\n")) {
        barrier_enter (pmi, client);
        if (zlist_size (pmi->barrier) == pmi->local_procs) {
            if (pmi->ops.barrier_enter)
                pmi->ops.barrier_enter (pmi->arg);
            else
                if (barrier_exit (pmi, 0) < 0)
                    rc = -1;
        }
        send_response = 0;
    } else if (!strcmp (buf, "cmd=finalize\n")) {
        snprintf (resp, sizeof (resp), "cmd=finalize_ack\n");
        rc = 1; /* Indicates fd should be closed */
    } else {
        errno = EPROTO;
        rc = -1;
        send_response = 0;
    }
    if (send_response) {
        if (pmi->debug)
            fprintf (stderr, "S: (client=%p) %s", client, resp);
        if (pmi->ops.response_send (client, resp) < 0)
            rc = -1;
    }
    return rc;
}

int pmi_simple_server_barrier_complete (struct pmi_simple_server *pmi, int rc)
{
    return barrier_exit (pmi, rc);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */