/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
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
#include <stdbool.h>
#include <jansson.h>

#include "src/common/libtap/tap.h"
#include "src/common/libkvs/kvs.h"
#include "src/common/libflux/message.h"
#include "src/common/libflux/request.h"
#include "src/modules/kvs/treq.h"
#include "ccan/str/str.h"

int msg_cb (treq_t *tr, const flux_msg_t *req, void *data)
{
    int *count = data;
    const char *topic;

    if (!flux_msg_get_topic (req, &topic)
        && streq (topic, "mytopic"))
        (*count)++;

    return 0;
}

int msg_cb_error (treq_t *tr, const flux_msg_t *req, void *data)
{
    return -1;
}

void treq_basic_tests (void)
{
    treq_t *tr;
    json_t *ops;
    json_t *o;
    flux_msg_t *request;
    const char *name;
    int count = 0;

    ok ((request = flux_request_encode ("mytopic", "{ bar : 1 }")) != NULL,
        "flux_request_encode works");

    ok ((tr = treq_create (request, 214, 3577, 3)) != NULL,
        "treq_create works");

    ok ((name = treq_get_name (tr)) != NULL,
        "treq_get_name works");

    ok (strstr (name, "214") != NULL,
        "treq_get_name returns name with rank in it");

    ok (strstr (name, "3577") != NULL,
        "treq_get_name returns name with seq in it");

    ok (treq_get_flags (tr) == 3,
        "treq_get_flags works");

    /* for test ops can be anything */
    ops = json_array ();
    json_array_append_new (ops, json_string ("A"));

    ok (treq_add_request_ops (tr, ops) == 0,
        "initial treq_add_request_ops add works");

    ok ((o = treq_get_ops (tr)) != NULL,
        "initial treq_get_ops call works");

    ok (json_equal (ops, o) == true,
        "initial treq_get_ops match");

    json_decref (ops);

    ok (treq_iter_request_copies (tr, msg_cb, &count) == 0,
        "second treq_iter_request_copies works");

    ok (count == 1,
        "second treq_iter_request_copies count is 1");

    ok (treq_get_processed (tr) == false,
        "treq_get_processed returns false initially");

    treq_mark_processed (tr);

    ok (treq_get_processed (tr) == true,
        "treq_get_processed returns true");

    flux_msg_destroy (request);

    treq_destroy (tr);
}

void treq_ops_tests (void)
{
    treq_t *tr;
    json_t *ops;
    json_t *o;

    ok ((tr = treq_create (NULL, 214, 3577, 3)) != NULL,
        "treq_create works");

    ok (treq_add_request_ops (tr, NULL) == 0,
        "treq_add_request_ops works with NULL ops");

    /* for test ops can be anything */
    ops = json_array ();
    json_array_append_new (ops, json_string ("A"));

    ok (treq_add_request_ops (tr, ops) == 0,
        "treq_add_request_ops add works");

    json_decref (ops);

    /* for test ops can be anything */
    ops = json_array ();
    json_array_append_new (ops, json_string ("B"));

    ok (treq_add_request_ops (tr, ops) == 0,
        "treq_add_request_ops add works");

    json_decref (ops);

    ok ((o = treq_get_ops (tr)) != NULL,
        "initial treq_get_ops call works");

    ops = json_array ();
    json_array_append_new (ops, json_string ("A"));
    json_array_append_new (ops, json_string ("B"));

    ok (json_equal (ops, o) == true,
        "treq_get_ops match");

    json_decref (ops);

    treq_destroy (tr);
}

void treq_request_tests (void)
{
    treq_t *tr;
    flux_msg_t *request;
    int count = 0;

    ok ((request = flux_request_encode ("mytopic", "{ A : 1 }")) != NULL,
        "flux_request_encode works");

    ok ((tr = treq_create (request, 214, 3577, 3)) != NULL,
        "treq_create works");

    flux_msg_destroy (request);

    ok (treq_iter_request_copies (tr, msg_cb_error, &count) == -1,
        "treq_iter_request_copies errors when cb errors");

    ok (treq_iter_request_copies (tr, msg_cb, &count) == 0,
        "second treq_iter_request_copies works");

    ok (count == 1,
        "treq_iter_request_copies count is 1");

    treq_destroy (tr);
}

void treq_mgr_basic_tests (void)
{
    treq_mgr_t *trm;
    treq_t *tr, *tmp_tr;
    const char *tmp_name;
    char *name;

    ok ((trm = treq_mgr_create ()) != NULL,
        "treq_mgr_create works");

    ok (treq_mgr_transactions_count (trm) == 0,
        "treq_mgr_transactions_count returns 0 when no transactions added");

    ok ((tr = treq_create (NULL, 214, 3577, 3)) != NULL,
        "treq_create works");

    ok ((tmp_name = treq_get_name (tr)) != NULL,
        "treq_get_name works");

    if (!(name = strdup (tmp_name)))
        BAIL_OUT ("strdup");

    ok (treq_mgr_add_transaction (trm, tr) == 0,
        "treq_mgr_add_transaction works");

    ok (treq_mgr_add_transaction (trm, tr) < 0,
        "treq_mgr_add_transaction fails on duplicate treq");

    ok ((tmp_tr = treq_mgr_lookup_transaction (trm, name)) != NULL,
        "treq_mgr_lookup_transaction works");

    ok (tr == tmp_tr,
        "treq_mgr_lookup_transaction returns correct treq");

    ok (treq_mgr_lookup_transaction (trm, "invalid") == NULL,
        "treq_mgr_lookup_transaction can't find invalid treq");

    ok (treq_mgr_transactions_count (trm) == 1,
        "treq_mgr_transactions_count returns 1 when treq submitted");

    treq_mgr_remove_transaction (trm, name);

    ok (treq_mgr_transactions_count (trm) == 0,
        "treq_mgr_transactions_count returns 0 after treq remove");

    ok (treq_mgr_lookup_transaction (trm, name) == NULL,
        "treq_mgr_lookup_transaction can't find removed treq");

    treq_mgr_destroy (trm);
    free (name);
}

int treq_count_cb (treq_t *tr, void *data)
{
    int *count = data;
    (*count)++;
    return 0;
}

int treq_remove_cb (treq_t *tr, void *data)
{
    treq_mgr_t *trm = data;

    treq_mgr_remove_transaction (trm, treq_get_name (tr));
    return 0;
}

int treq_add_error_cb (treq_t *tr, void *data)
{
    treq_mgr_t *trm = data;
    treq_t *tr2;

    if (!(tr2 = treq_create (NULL, 123, 456, 7)))
        BAIL_OUT ("treq_create");

    if (treq_mgr_add_transaction (trm, tr2) < 0) {
        treq_destroy (tr2);
        return -1;
    }
    return 0;
}

int treq_error_cb (treq_t *tr, void *data)
{
    return -1;
}

void treq_mgr_iter_tests (void)
{
    treq_mgr_t *trm;
    treq_t *tr;
    int count;

    ok ((trm = treq_mgr_create ()) != NULL,
        "treq_mgr_create works");

    count = 0;
    ok (treq_mgr_iter_transactions (trm, treq_count_cb, &count) == 0
        && count == 0,
        "treq_mgr_iter_transactions success when no transactions submitted");

    ok ((tr = treq_create (NULL, 214, 3577, 3)) != NULL,
        "treq_create works");

    ok (treq_mgr_add_transaction (trm, tr) == 0,
        "treq_mgr_add_transaction works");

    ok (treq_mgr_transactions_count (trm) == 1,
        "treq_mgr_transactions_count returns correct count of transactions");

    ok (treq_mgr_iter_transactions (trm, treq_error_cb, NULL) < 0,
        "treq_mgr_iter_transactions error on callback error");

    ok (treq_mgr_iter_transactions (trm, treq_add_error_cb, trm) < 0
        && errno == EAGAIN,
        "treq_mgr_iter_transactions error on callback error trying to add treq");

    ok (treq_mgr_iter_transactions (trm, treq_remove_cb, trm) == 0,
        "treq_mgr_iter_transactions success on remove");

    count = 0;
    ok (treq_mgr_iter_transactions (trm, treq_count_cb, &count) == 0,
        "treq_mgr_iter_transactions success on count");

    ok (count == 0,
        "treq_mgr_iter_transactions returned correct count of transactions");

    ok (treq_mgr_transactions_count (trm) == 0,
        "treq_mgr_transactions_count returns correct count of transactions");

    treq_mgr_destroy (trm);
}

int main (int argc, char *argv[])
{
    plan (NO_PLAN);

    treq_basic_tests ();
    treq_ops_tests ();
    treq_request_tests ();
    treq_mgr_basic_tests ();
    treq_mgr_iter_tests ();

    done_testing ();
    return (0);
}


/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
