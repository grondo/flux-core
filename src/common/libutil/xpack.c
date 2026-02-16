#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <jansson.h>

#include "fluid.h"
#include "xpack.h"

#define FLUX_JOBID_ENCODE_MAX 32

/* Generate wrapper macros using X-macros for different argument counts.
 * This avoids manually typing out all 16 variations.
 */

/* Helper macro to expand arguments */
#define ARGN_0()
#define ARGN_1()  , a1
#define ARGN_2()  , a1, a2
#define ARGN_3()  , a1, a2, a3
#define ARGN_4()  , a1, a2, a3, a4
#define ARGN_5()  , a1, a2, a3, a4, a5
#define ARGN_6()  , a1, a2, a3, a4, a5, a6
#define ARGN_7()  , a1, a2, a3, a4, a5, a6, a7
#define ARGN_8()  , a1, a2, a3, a4, a5, a6, a7, a8
#define ARGN_9()  , a1, a2, a3, a4, a5, a6, a7, a8, a9
#define ARGN_10() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10
#define ARGN_11() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11
#define ARGN_12() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12
#define ARGN_13() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13
#define ARGN_14() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, \
                    a13, a14
#define ARGN_15() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, \
                    a13, a14, a15
#define ARGN_16() , a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, \
                    a13, a14, a15, a16

/* Generate CALL_JSON_VPACK_N macros */
#define CALL_JSON_VPACK_0(err, flags, fmt) \
    json_pack_ex (err, flags, fmt)

#define DEFINE_CALL_JSON_VPACK(N) \
    static inline json_t *call_json_vpack_##N ( \
        json_error_t *err, \
        size_t flags, \
        const char *fmt, \
        void *a1, void *a2, void *a3, void *a4, \
        void *a5, void *a6, void *a7, void *a8, \
        void *a9, void *a10, void *a11, void *a12, \
        void *a13, void *a14, void *a15, void *a16) \
    { \
        return json_pack_ex (err, flags, fmt ARGN_##N ()); \
    }

/* Generate functions for 1-16 arguments */
DEFINE_CALL_JSON_VPACK (1)
DEFINE_CALL_JSON_VPACK (2)
DEFINE_CALL_JSON_VPACK (3)
DEFINE_CALL_JSON_VPACK (4)
DEFINE_CALL_JSON_VPACK (5)
DEFINE_CALL_JSON_VPACK (6)
DEFINE_CALL_JSON_VPACK (7)
DEFINE_CALL_JSON_VPACK (8)
DEFINE_CALL_JSON_VPACK (9)
DEFINE_CALL_JSON_VPACK (10)
DEFINE_CALL_JSON_VPACK (11)
DEFINE_CALL_JSON_VPACK (12)
DEFINE_CALL_JSON_VPACK (13)
DEFINE_CALL_JSON_VPACK (14)
DEFINE_CALL_JSON_VPACK (15)
DEFINE_CALL_JSON_VPACK (16)

/* Generate CALL_JSON_VUNPACK_N macros */
#define CALL_JSON_VUNPACK_0(root, err, flags, fmt) \
    json_unpack_ex (root, err, flags, fmt)

#define DEFINE_CALL_JSON_VUNPACK(N) \
    static inline int call_json_vunpack_##N ( \
        json_t *root, \
        json_error_t *err, \
        size_t flags, \
        const char *fmt, \
        void *a1, void *a2, void *a3, void *a4, \
        void *a5, void *a6, void *a7, void *a8, \
        void *a9, void *a10, void *a11, void *a12, \
        void *a13, void *a14, void *a15, void *a16) \
    { \
        return json_unpack_ex (root, err, flags, fmt ARGN_##N ()); \
    }

/* Generate functions for 1-16 arguments */
DEFINE_CALL_JSON_VUNPACK (1)
DEFINE_CALL_JSON_VUNPACK (2)
DEFINE_CALL_JSON_VUNPACK (3)
DEFINE_CALL_JSON_VUNPACK (4)
DEFINE_CALL_JSON_VUNPACK (5)
DEFINE_CALL_JSON_VUNPACK (6)
DEFINE_CALL_JSON_VUNPACK (7)
DEFINE_CALL_JSON_VUNPACK (8)
DEFINE_CALL_JSON_VUNPACK (9)
DEFINE_CALL_JSON_VUNPACK (10)
DEFINE_CALL_JSON_VUNPACK (11)
DEFINE_CALL_JSON_VUNPACK (12)
DEFINE_CALL_JSON_VUNPACK (13)
DEFINE_CALL_JSON_VUNPACK (14)
DEFINE_CALL_JSON_VUNPACK (15)
DEFINE_CALL_JSON_VUNPACK (16)

/* Macro to generate switch case for pack */
#define CASE_CALL_VPACK(N) \
    case N: \
        result = call_json_vpack_##N ( \
            error, \
            flags, \
            args.fmt, \
            args.args[0], args.args[1], args.args[2], args.args[3], \
            args.args[4], args.args[5], args.args[6], args.args[7], \
            args.args[8], args.args[9], args.args[10], args.args[11], \
            args.args[12], args.args[13], args.args[14], args.args[15]); \
        break;

/* Macro to generate switch case for unpack */
#define CASE_CALL_VUNPACK(N) \
    case N: \
        rc = call_json_vunpack_##N ( \
            root, \
            error, \
            flags, \
            args.fmt, \
            args.args[0], args.args[1], args.args[2], args.args[3], \
            args.args[4], args.args[5], args.args[6], args.args[7], \
            args.args[8], args.args[9], args.args[10], args.args[11], \
            args.args[12], args.args[13], args.args[14], args.args[15]); \
        break;

#define EXTENDED_PACK_MAX_ARGS 16

/* Structure to hold transformed pack arguments */
struct pack_args {
    void *args[EXTENDED_PACK_MAX_ARGS];
    char *strings[EXTENDED_PACK_MAX_ARGS];  /* Temporary encoded strings */
    int nargs;
    int nstrings;
    char fmt[1024];
};

/* Structure to hold transformed unpack arguments */
struct unpack_args {
    void *args[EXTENDED_PACK_MAX_ARGS];
    const char *strings[EXTENDED_PACK_MAX_ARGS];
    fluid_t *fluids[EXTENDED_PACK_MAX_ARGS];
    int nargs;
    int ncustom;
    char fmt[1024];
};

/* Format token types */
enum token_type {
    TOKEN_CHAR,           /* Single character (no args or 1 arg) */
    TOKEN_STRING_LEN,     /* s# or s% or +# or +% (2 args) */
    TOKEN_CUSTOM,         /* J (custom type) */
    TOKEN_UNKNOWN,        /* Unknown format specifier */
    TOKEN_OTHER,          /* Non-consuming character */
};

/* Scan next token from format string.
 * Returns token type and advances *fmtp past the token.
 * For TOKEN_CHAR and TOKEN_CUSTOM, sets *spec to the specifier character.
 * For TOKEN_STRING_LEN, sets *spec to '#' or '%'.
 * For TOKEN_UNKNOWN, sets *spec to the unknown character.
 *
 * Known jansson 2.12-2.15 format specifiers:
 *   Pack: s, i, I, b, f, o, O, n, +
 *   Unpack: s, i, I, b, f, F, o, O, n
 *   Both: s#, s%, +#, +% (2 args)
 *   Modifiers: ?, * (after s, o, O)
 *   Non-consuming: n, !, *
 *
 * This function validates against jansson 2.15. If a newer version of
 * jansson adds format specifiers, this will return TOKEN_UNKNOWN to
 * prevent undefined behavior from consuming wrong number of va_list args.
 */
static enum token_type scan_format_token (const char **fmtp, char *spec)
{
    const char *fmt = *fmtp;

    *spec = 0;

    if (!*fmt)
        return TOKEN_OTHER;

    /* Check for two-character specifiers first */
    if (*fmt == 's' && (*(fmt + 1) == '#' || *(fmt + 1) == '%')) {
        *spec = *(fmt + 1);
        *fmtp = fmt + 2;
        return TOKEN_STRING_LEN;
    }
    else if (*fmt == '+' && (*(fmt + 1) == '#' || *(fmt + 1) == '%')) {
        *spec = *(fmt + 1);
        *fmtp = fmt + 2;
        return TOKEN_STRING_LEN;
    }
    else if (*fmt == 'J') {
        *spec = 'J';
        *fmtp = fmt + 1;
        return TOKEN_CUSTOM;
    }
    else if (*fmt == 'n' || *fmt == '!' || *fmt == '*') {
        /* Non-consuming specifiers */
        *spec = *fmt;
        *fmtp = fmt + 1;
        return TOKEN_OTHER;
    }
    else if (strchr ("ioObfFIs+", *fmt)) {
        /* Note: F is unpack-only, + is pack-only, but we accept both
         * since this scanner is used for both operations */
        *spec = *fmt;
        *fmtp = fmt + 1;
        return TOKEN_CHAR;
    }
    /* Check for unknown format specifiers (potential jansson additions).
     * Any uppercase letter (except known ones) or lowercase letter
     * (except known ones) that could be a format specifier.
     */
    else if (isupper (*fmt) || islower (*fmt)) {
        *spec = *fmt;
        *fmtp = fmt + 1;
        return TOKEN_UNKNOWN;
    }
    else {
        *spec = *fmt;
        *fmtp = fmt + 1;
        return TOKEN_OTHER;
    }
}

/* Check if format string contains custom type specifier */
static bool has_custom_type (const char *fmt, char type)
{
    while (*fmt) {
        if (*fmt == type)
            return true;
        fmt++;
    }
    return false;
}

/* Transform pack arguments: extract from va_list, encode custom types,
 * build new format.
 * Caller must have called va_copy on ap.
 */
static int transform_pack_args (const char *fmt,
                                va_list ap,
                                struct pack_args *out,
                                json_error_t *error)
{
    const char *p = fmt;
    char *outfmt = out->fmt;
    char spec;
    enum token_type type;

    out->nargs = 0;
    out->nstrings = 0;

    while ((type = scan_format_token (&p, &spec)) != TOKEN_OTHER || spec) {
        if (out->nargs >= EXTENDED_PACK_MAX_ARGS) {
            if (error)
                snprintf (error->text,
                          sizeof (error->text),
                          "too many arguments (max %d)",
                          EXTENDED_PACK_MAX_ARGS);
            return -1;
        }

        switch (type) {
        case TOKEN_UNKNOWN:
            if (error)
                snprintf (error->text,
                          sizeof (error->text),
                          "unknown format specifier '%c' "
                          "(jansson version mismatch?)",
                          spec);
            return -1;

        case TOKEN_CUSTOM:
            if (spec == 'J') {
                /* Custom jobid type - encode to string */
                fluid_t id = va_arg (ap, fluid_t);
                int len = FLUX_JOBID_ENCODE_MAX;
                char *buf = malloc (len);

                if (!buf) {
                    if (error)
                        snprintf (error->text,
                                  sizeof (error->text),
                                  "out of memory");
                    return -1;
                }

                if (fluid_encode (buf, len, id, FLUID_STRING_F58) < 0) {
                    if (error) {
                        snprintf (error->text,
                                  sizeof (error->text),
                                  "fluid_encode: %s",
                                  strerror (errno));
                    }
                    free (buf);
                    return -1;
                }

                /* Store encoded string and add to args */
                out->strings[out->nstrings++] = buf;
                out->args[out->nargs++] = buf;

                /* Replace J with s in format */
                *outfmt++ = 's';
            }
            break;

        case TOKEN_STRING_LEN:
            /* s# or s% or +# or +% - string with length, takes 2 args */
            out->args[out->nargs++] = va_arg (ap, void *);
            out->args[out->nargs++] = va_arg (ap, void *);
            *outfmt++ = 's';
            *outfmt++ = spec;
            break;

        case TOKEN_CHAR:
            /* Regular argument-consuming character */
            out->args[out->nargs++] = va_arg (ap, void *);
            *outfmt++ = spec;
            break;

        case TOKEN_OTHER:
            /* Non-argument character, just copy */
            if (spec)
                *outfmt++ = spec;
            break;
        }
    }

    *outfmt = '\0';
    return 0;
}

/* Free temporary strings allocated during pack */
static void pack_args_cleanup (struct pack_args *args)
{
    for (int i = 0; i < args->nstrings; i++)
        free (args->strings[i]);
}

/* Transform unpack arguments: replace J with s, save original pointers.
 * Caller must have called va_copy on ap.
 */
static int transform_unpack_args (const char *fmt,
                                  va_list ap,
                                  struct unpack_args *out,
                                  json_error_t *error)
{
    const char *p = fmt;
    char *outfmt = out->fmt;
    char spec;
    enum token_type type;

    out->nargs = 0;
    out->ncustom = 0;

    while ((type = scan_format_token (&p, &spec)) != TOKEN_OTHER || spec) {
        if (out->nargs >= EXTENDED_PACK_MAX_ARGS) {
            if (error)
                snprintf (error->text,
                          sizeof (error->text),
                          "too many arguments (max %d)",
                          EXTENDED_PACK_MAX_ARGS);
            return -1;
        }

        switch (type) {
        case TOKEN_UNKNOWN:
            if (error)
                snprintf (error->text,
                          sizeof (error->text),
                          "unknown format specifier '%c' "
                          "(jansson version mismatch?)",
                          spec);
            return -1;

        case TOKEN_CUSTOM:
            if (spec == 'J') {
                /* Custom jobid type - will unpack as string */
                fluid_t *idp = va_arg (ap, fluid_t *);
                const char **temp = &out->strings[out->ncustom];

                /* Save original pointer for later decoding */
                out->fluids[out->ncustom] = idp;
                out->ncustom++;

                /* Add temp string pointer to args */
                out->args[out->nargs++] = temp;

                /* Replace J with s in format */
                *outfmt++ = 's';
            }
            break;

        case TOKEN_STRING_LEN:
            /* s# or s% or +# or +% - string with length, takes 2 args */
            out->args[out->nargs++] = va_arg (ap, void *);
            out->args[out->nargs++] = va_arg (ap, void *);
            *outfmt++ = 's';
            *outfmt++ = spec;
            break;

        case TOKEN_CHAR:
            /* Regular argument-consuming character */
            out->args[out->nargs++] = va_arg (ap, void *);
            *outfmt++ = spec;
            break;

        case TOKEN_OTHER:
            /* Non-argument character, just copy */
            if (spec)
                *outfmt++ = spec;
            break;
        }
    }

    *outfmt = '\0';
    return 0;
}

/* Decode custom types after unpacking */
static int unpack_decode_custom (struct unpack_args *args,
                                 json_error_t *error)
{
    for (int i = 0; i < args->ncustom; i++) {
        const char *s = args->strings[i];
        fluid_t *idp = args->fluids[i];

        if (s && fluid_parse (s, idp) < 0) {
            if (error) {
                snprintf (error->text,
                          sizeof (error->text),
                          "fluid_parse(%s): %s",
                          s,
                          strerror (errno));
            }
            return -1;
        }
    }
    return 0;
}

json_t *xvpack_ex (json_error_t *error,
                   size_t flags,
                   const char *fmt,
                   va_list ap)
{
    struct pack_args args = {0};
    json_t *result = NULL;
    va_list ap_copy;

    if (!fmt) {
        if (error)
            snprintf (error->text,
                      sizeof (error->text),
                      "NULL format string");
        errno = EINVAL;
        return NULL;
    }

    /* Fast path: no custom types, use jansson directly */
    if (!has_custom_type (fmt, 'J'))
        return json_vpack_ex (error, flags, fmt, ap);

    /* Transform arguments */
    va_copy (ap_copy, ap);
    if (transform_pack_args (fmt, ap_copy, &args, error) < 0) {
        va_end (ap_copy);
        goto done;
    }
    va_end (ap_copy);

    /* Call json_pack_ex with transformed arguments */
    switch (args.nargs) {
    case 0:
        result = CALL_JSON_VPACK_0 (error, flags, args.fmt);
        break;
    CASE_CALL_VPACK (1)
    CASE_CALL_VPACK (2)
    CASE_CALL_VPACK (3)
    CASE_CALL_VPACK (4)
    CASE_CALL_VPACK (5)
    CASE_CALL_VPACK (6)
    CASE_CALL_VPACK (7)
    CASE_CALL_VPACK (8)
    CASE_CALL_VPACK (9)
    CASE_CALL_VPACK (10)
    CASE_CALL_VPACK (11)
    CASE_CALL_VPACK (12)
    CASE_CALL_VPACK (13)
    CASE_CALL_VPACK (14)
    CASE_CALL_VPACK (15)
    CASE_CALL_VPACK (16)
    default:
        if (error)
            snprintf (error->text,
                      sizeof (error->text),
                      "too many arguments (max %d)",
                      EXTENDED_PACK_MAX_ARGS);
        errno = EINVAL;
        break;
    }

done:
    pack_args_cleanup (&args);
    return result;
}

json_t *xpack_ex (json_error_t *error,
                  size_t flags,
                  const char *fmt,
                  ...)
{
    va_list ap;
    json_t *result;

    va_start (ap, fmt);
    result = xvpack_ex (error, flags, fmt, ap);
    va_end (ap);

    return result;
}

json_t *xpack (const char *fmt, ...)
{
    va_list ap;
    json_t *result;

    va_start (ap, fmt);
    result = xvpack_ex (NULL, 0, fmt, ap);
    va_end (ap);

    return result;
}

int xvunpack_ex (json_t *root,
                 json_error_t *error,
                 size_t flags,
                 const char *fmt,
                 va_list ap)
{
    struct unpack_args args = {0};
    int rc = -1;
    va_list ap_copy;

    if (!root || !fmt) {
        if (error)
            snprintf (error->text,
                      sizeof (error->text),
                      "NULL %s",
                      !root ? "root" : "format string");
        errno = EINVAL;
        return -1;
    }

    /* Fast path: no custom types, use jansson directly */
    if (!has_custom_type (fmt, 'J'))
        return json_vunpack_ex (root, error, flags, fmt, ap);

    /* Transform arguments */
    va_copy (ap_copy, ap);
    if (transform_unpack_args (fmt, ap_copy, &args, error) < 0) {
        va_end (ap_copy);
        return -1;
    }
    va_end (ap_copy);

    /* Call json_unpack_ex with transformed arguments */
    switch (args.nargs) {
    case 0:
        rc = CALL_JSON_VUNPACK_0 (root, error, flags, args.fmt);
        break;
    CASE_CALL_VUNPACK (1)
    CASE_CALL_VUNPACK (2)
    CASE_CALL_VUNPACK (3)
    CASE_CALL_VUNPACK (4)
    CASE_CALL_VUNPACK (5)
    CASE_CALL_VUNPACK (6)
    CASE_CALL_VUNPACK (7)
    CASE_CALL_VUNPACK (8)
    CASE_CALL_VUNPACK (9)
    CASE_CALL_VUNPACK (10)
    CASE_CALL_VUNPACK (11)
    CASE_CALL_VUNPACK (12)
    CASE_CALL_VUNPACK (13)
    CASE_CALL_VUNPACK (14)
    CASE_CALL_VUNPACK (15)
    CASE_CALL_VUNPACK (16)
    default:
        if (error)
            snprintf (error->text,
                      sizeof (error->text),
                      "too many arguments (max %d)",
                      EXTENDED_PACK_MAX_ARGS);
        errno = EINVAL;
        return -1;
    }

    /* Decode custom types if unpack succeeded */
    if (rc == 0 && args.ncustom > 0) {
        if (unpack_decode_custom (&args, error) < 0)
            rc = -1;
    }

    if (rc < 0)
        errno = EPROTO;

    return rc;
}

int xunpack_ex (json_t *root,
                json_error_t *error,
                size_t flags,
                const char *fmt,
                ...)
{
    va_list ap;
    int rc;

    va_start (ap, fmt);
    rc = xvunpack_ex (root, error, flags, fmt, ap);
    va_end (ap);

    return rc;
}

int xunpack (json_t *root, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start (ap, fmt);
    rc = xvunpack_ex (root, NULL, 0, fmt, ap);
    va_end (ap);

    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
