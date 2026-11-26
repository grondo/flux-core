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

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#if HAVE_FLUX_SECURITY
#include <flux/security/context.h>
#include <flux/security/sign.h>
#endif

#include "src/common/libutil/errprintf.h"

#include "unwrap.h"
#include "sign_none.h"

char *unwrap_string_sign_none (const char *s,
                               bool verify,
                               uint32_t *userid,
                               flux_error_t *errp)
{
    char *result = NULL;
    int len;
    uint32_t uid;
    void *data = NULL;
    if (sign_none_unwrap (s, (void **) &data, &len, &uid) < 0) {
        errprintf (errp, "sign-none-unwrap failed: %s", strerror (errno));
        return NULL;
    }
    if (verify && uid != getuid ()) {
        errprintf (errp,
                   "sign-none-unwrap: signing userid %lu != current %lu",
                   (unsigned long) uid,
                   (unsigned long) getuid ());
        free (data);
        return NULL;
    }
    /*  Add one extra byte to ensure NUL termination
     */
    if (!(result = calloc (1, len+1))) {
        errprintf (errp, "Out of memory");
        goto out;
    }
    memcpy (result, data, len);
    if (userid)
        *userid = uid;
out:
    free (data);
    return result;
}

#if HAVE_FLUX_SECURITY
static char *unwrap_string (const char *s,
                            int flags,
                            bool anymech,
                            uint32_t *userid,
                            flux_error_t *errp)
{
    flux_security_t *sec;
    const void *data = NULL;
    char *result = NULL;
    int len;
    const char *mech;
    int64_t userid64;
    int rc;

    if (!(sec = flux_security_create (0))) {
        errprintf (errp,
                   "failed to create security context: %s",
                   strerror (errno));
        return NULL;
    }
    if (flux_security_configure (sec, NULL) < 0) {
        errprintf (errp,
                   "failed to configure security context: %s",
                   flux_security_last_error (sec));
        goto done;
    }
    if (anymech)
        rc = flux_sign_unwrap_anymech (sec,
                                       s,
                                       &data,
                                       &len,
                                       &mech,
                                       &userid64,
                                       flags);
    else
        rc = flux_sign_unwrap (sec, s, &data, &len, &userid64, flags);
    if (rc < 0) {
        errprintf (errp, "%s", flux_security_last_error (sec));
        goto done;
    }
    /*  Add one extra byte to ensure NUL termination in case NUL byte
     *   not included in len:
     */
    if (!(result = calloc (1, len+1))) {
        errprintf (errp, "Out of memory");
        goto done;
    }
    memcpy (result, data, len);
    if (userid)
        *userid = (uint32_t) userid64;
done:
    flux_security_destroy (sec);
    return result;
}
#endif /* HAVE_FLUX_SECURITY */

char *flux_unwrap_string (const char *s,
                          bool verify,
                          uint32_t *userid,
                          flux_error_t *errp)
{
#if HAVE_FLUX_SECURITY
    return unwrap_string (s,
                          verify ? 0 : FLUX_SIGN_NOVERIFY,
                          true,
                          userid,
                          errp);
#else
    return unwrap_string_sign_none (s, verify, userid, errp);
#endif /* !HAVE_FLUX_SECURITY */
}

char *unwrap_string_verify_allowed (const char *s,
                                    uint32_t *userid,
                                    flux_error_t *errp)
{
#if HAVE_FLUX_SECURITY
    return unwrap_string (s, 0, false, userid, errp);
#else
    return unwrap_string_sign_none (s, true, userid, errp);
#endif /* !HAVE_FLUX_SECURITY */
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
