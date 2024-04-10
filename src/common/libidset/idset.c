/************************************************************\
 * Copyright 2018 Lawrence Livermore National Security, LLC
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
#include <sys/param.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include "idset.h"
#include "idset_private.h"

int validate_idset_flags (int flags, int allowed)
{
    if ((flags & allowed) != flags) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

struct idset *idset_create (size_t size, int flags)
{
    struct idset *idset;
    int valid_flags = IDSET_FLAG_AUTOGROW
                    | IDSET_FLAG_INITFULL
                    | IDSET_FLAG_COUNT_LAZY;

    if (validate_idset_flags (flags, valid_flags) < 0)
        return NULL;
    if (size == 0)
        size = IDSET_DEFAULT_SIZE;
    if (!(idset = malloc (sizeof (*idset))))
        return NULL;
    if ((flags & IDSET_FLAG_INITFULL))
        idset->b = roaring_bitmap_from_range (0, size, 1);
    else
        idset->b = roaring_bitmap_create ();
    if (!idset->b) {
        free (idset);
        errno = ENOMEM;
        return NULL;
    }
    idset->flags = flags;
    idset->size = size;
    return idset;
}

void idset_destroy (struct idset *idset)
{
    if (idset) {
        int saved_errno = errno;
        roaring_bitmap_free (idset->b);
        free (idset);
        errno = saved_errno;
    }
}

size_t idset_universe_size (const struct idset *idset)
{
    return idset ? idset->size : 0;
}

static struct idset *idset_copy_flags (const struct idset *idset, int flags)
{
    struct idset *cpy;

    if (!(cpy = malloc (sizeof (*idset))))
        return NULL;
    cpy->flags = flags;
    cpy->b = roaring_bitmap_copy (idset->b);
    if (!cpy->b) {
        idset_destroy (cpy);
        return NULL;
    }
    cpy->size = idset->size;
    return cpy;
}

struct idset *idset_copy (const struct idset *idset)
{
    if (!idset) {
        errno = EINVAL;
        return NULL;
    }
    return idset_copy_flags (idset, idset->flags);
}

static bool valid_id (unsigned int id)
{
    if (id == UINT_MAX || id == IDSET_INVALID_ID)
        return false;
    return true;
}

static int idset_grow (struct idset *idset, size_t size)
{
    size_t newsize = idset->size;

    while (newsize < size)
        newsize <<= 1;

    if (newsize > idset->size) {
        if (!(idset->flags & IDSET_FLAG_AUTOGROW)) {
            errno = EINVAL;
            return -1;
        }
        if ((idset->flags & IDSET_FLAG_INITFULL))
            roaring_bitmap_add_range (idset->b, idset->size, newsize);
        idset->size = newsize;
    }
    return 0;
}

int idset_set (struct idset *idset, unsigned int id)
{
    if (!idset || !valid_id (id)) {
        errno = EINVAL;
        return -1;
    }
    if (id >= idset_universe_size (idset)) {
        /* N.B. we do not try to grow the idset to accommodate out of range ids
         * when the operation is 'set' and IDSET_FLAG_INITFULL is set.
         * Treat it as a successful no-op.
         */
        if ((idset->flags & IDSET_FLAG_INITFULL))
            return 0;
        if (idset_grow (idset, id + 1) < 0)
            return -1;
    }
    roaring_bitmap_add (idset->b, id);
    return 0;
}

static void normalize_range (unsigned int *lo, unsigned int *hi)
{
    if (*hi < *lo) {
        unsigned int tmp = *hi;
        *hi = *lo;
        *lo = tmp;
    }
}

int idset_range_set (struct idset *idset, unsigned int lo, unsigned int hi)
{
    if (!idset || !valid_id (lo) || !valid_id (hi)) {
        errno = EINVAL;
        return -1;
    }
    normalize_range (&lo, &hi);

    if (!(idset->flags & IDSET_FLAG_INITFULL)) {
        if (idset_grow (idset, hi + 1) < 0)
            return -1;
    }
    else if (hi >= idset_universe_size (idset))
        hi = idset_universe_size (idset) - 1;

    roaring_bitmap_add_range_closed (idset->b, lo, hi);
    return 0;
}

int idset_clear (struct idset *idset, unsigned int id)
{
    if (!idset || !valid_id (id)) {
        errno = EINVAL;
        return -1;
    }
    if (id >= idset_universe_size (idset)) {
        /* N.B. we do not try to grow the idset to accommodate out of range ids
         * when the operation is 'clear' and IDSET_FLAG_INITFULL is NOT set.
         * Treat this as a successful no-op.
         */
        if (!(idset->flags & IDSET_FLAG_INITFULL))
            return 0;
        if (idset_grow (idset, id + 1) < 0)
            return -1;
    }
    roaring_bitmap_remove (idset->b, id);
    return 0;
}

int idset_range_clear (struct idset *idset, unsigned int lo, unsigned int hi)
{
    if (!idset || !valid_id (lo) || !valid_id (hi)) {
        errno = EINVAL;
        return -1;
    }
    normalize_range (&lo, &hi);
    if (hi >= idset_universe_size (idset)
        && !(idset->flags & IDSET_FLAG_INITFULL))
        hi = idset_universe_size (idset) - 1;
    roaring_bitmap_remove_range_closed (idset->b, lo, hi);
    return 0;
}

bool idset_test (const struct idset *idset, unsigned int id)
{
    if (!idset || !valid_id (id))
        return false;
    return roaring_bitmap_contains (idset->b, id);
}

unsigned int idset_first (const struct idset *idset)
{
    unsigned int next = IDSET_INVALID_ID;
    if (idset && !roaring_bitmap_is_empty (idset->b))
        next = roaring_bitmap_minimum (idset->b);
    return next;
}


unsigned int idset_next (const struct idset *idset, unsigned int id)
{
    unsigned int next = IDSET_INVALID_ID;

    if (idset) {
        roaring_uint32_iterator_t i;
        roaring_iterator_init (idset->b, &i);
        roaring_uint32_iterator_move_equalorlarger (&i, id+1);
        if (i.has_value)
            next = i.current_value;
    }
    return next;
}

unsigned int idset_last (const struct idset *idset)
{
    unsigned int last = IDSET_INVALID_ID;

    if (idset && !roaring_bitmap_is_empty (idset->b))
        last = roaring_bitmap_maximum (idset->b);
    return last;
}

unsigned int idset_prev (const struct idset *idset, unsigned int id)
{
    unsigned int next = IDSET_INVALID_ID;

    if (id == IDSET_INVALID_ID)
        return next;

    if (idset) {
        roaring_uint32_iterator_t i;
        roaring_iterator_init (idset->b, &i);
        roaring_uint32_iterator_move_equalorlarger (&i, id);
        roaring_uint32_iterator_previous (&i);
        if (i.has_value)
            next = i.current_value;
    }
    return next;
}

size_t idset_count (const struct idset *idset)
{
    if (!idset)
        return 0;
    return (size_t) roaring_bitmap_get_cardinality (idset->b);
}

bool idset_empty (const struct idset *idset)
{
    if (!idset || roaring_bitmap_is_empty (idset->b))
        return true;
    return false;
}

bool idset_equal (const struct idset *idset1,
                  const struct idset *idset2)
{
    if (!idset1 || !idset2)
        return false;
    return roaring_bitmap_equals (idset1->b, idset2->b);
}

bool idset_has_intersection (const struct idset *a, const struct idset *b)
{
    if (a && b)
        return roaring_bitmap_intersect (a->b, b->b);
    return false;
}

int idset_add (struct idset *a, const struct idset *b)
{
    if (!a) {
        errno = EINVAL;
        return -1;
    }
    unsigned int last = idset_last (b);
    if (last >= idset_universe_size (a))
        idset_grow (a, last + 1);
    if (b)
        roaring_bitmap_or_inplace (a->b, b->b);
    return 0;
}

struct idset *idset_union (const struct idset *a, const struct idset *b)
{
    struct idset *result;

    if (!a) {
        errno = EINVAL;
        return NULL;
    }
    if (!(result = idset_copy_flags (a, IDSET_FLAG_AUTOGROW)))
        return NULL;
    if (idset_add (result, b) < 0) {
        idset_destroy (result);
        return NULL;
    }
    return result;
}

int idset_subtract (struct idset *a, const struct idset *b)
{
    if (!a) {
        errno = EINVAL;
        return -1;
    }
    if (a == b)
        roaring_bitmap_clear (a->b);
    else if (b)
        roaring_bitmap_andnot_inplace (a->b, b->b);
    return 0;
}

struct idset *idset_difference (const struct idset *a, const struct idset *b)
{
    struct idset *result;

    if (!a) {
        errno = EINVAL;
        return NULL;
    }
    if (!(result = idset_copy (a)))
        return NULL;
    if (idset_subtract (result, b) < 0) {
        idset_destroy (result);
        return NULL;
    }
    return result;
}

struct idset *idset_intersect (const struct idset *a, const struct idset *b)
{
    struct idset *result;

    if (!a || !b) {
        errno = EINVAL;
        return NULL;
    }
    if (!(result = idset_copy (a)))
        return NULL;
    roaring_bitmap_and_inplace (result->b, b->b);
    return result;
}

/* Find the next available id.  If there isn't one, try to grow the set.
 * The grow attempt will fail if IDSET_FLAG_AUTOGROW is not set.
 * Finally call vebdel() to take the id out of the set and return it.
 */
int idset_alloc (struct idset *idset, unsigned int *val)
{
    unsigned int id;

    if (!idset || !(idset->flags & IDSET_FLAG_INITFULL) || !val) {
        errno = EINVAL;
        return -1;
    }
    id = idset_first (idset);
    if (id == IDSET_INVALID_ID) {
        id = idset_universe_size (idset);
        if (idset_grow (idset, id + 1) < 0)
            return -1;
    }
    idset_clear (idset, id);
    *val = id;
    return 0;
}

/* Return an id to the set, ignoring invalid or out of range ones.
 * This does not catch double-frees.
 */
void idset_free (struct idset *idset, unsigned int val)
{
    if (!idset || !(idset->flags & IDSET_FLAG_INITFULL))
        return;
    idset_set (idset, val);
}

/* Same as above but fail if the id is already in the set.
 */
int idset_free_check (struct idset *idset, unsigned int val)
{
    if (!idset
        || !(idset->flags & IDSET_FLAG_INITFULL)
        || !valid_id (val)
        || val >= idset_universe_size (idset)) {
        errno = EINVAL;
        return -1;
    }
    if (idset_test (idset, val)) {
        errno = EEXIST;
        return -1;
    }
    // code above ensures that id is NOT a member of idset
    idset_set (idset, val);
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
