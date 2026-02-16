/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _XPACK_H
#define _XPACK_H

#include <stdarg.h>
#include <jansson.h>

/* Drop-in replacements for json_vpack_ex/json_vunpack_ex that support
 * custom Flux type specifiers in addition to standard jansson types.
 *
 * Custom specifiers:
 *   J (jobid) [flux_jobid_t / flux_jobid_t *]
 *      Pack: Encode flux_jobid_t to string (f58 encoding)
 *      Unpack: Decode string to flux_jobid_t
 *      Supports optional form: s?J
 */

json_t *xpack (const char *fmt, ...);
json_t *xpack_ex (json_error_t *error,
                  size_t flags,
                  const char *fmt,
                  ...);
json_t *xvpack_ex (json_error_t *error,
                   size_t flags,
                   const char *fmt,
                   va_list ap);

int xunpack (json_t *root, const char *fmt, ...);
int xunpack_ex (json_t *root,
                json_error_t *error,
                size_t flags,
                const char *fmt,
                ...);
int xvunpack_ex (json_t *root,
                 json_error_t *error,
                 size_t flags,
                 const char *fmt,
                 va_list ap);

#endif /* !_XPACK_H */
