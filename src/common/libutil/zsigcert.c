/* zsigcert - work with CURVE signing certificates
 * Derived from:
 */
/* =========================================================================
 * zcert - work with CURVE security certificates
 *
 * Copyright (c) the Contributors as noted in the AUTHORS file.
 * This file is part of CZMQ, the high-level C binding for 0MQ:
 * http://czmq.zeromq.org.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * =========================================================================
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <czmq.h>
#include <sodium.h>

#include "zsigcert.h"
#include "base64.h"

#define PUBSIZE crypto_sign_PUBLICKEYBYTES
#define SECSIZE crypto_sign_SECRETKEYBYTES
#define SIGSIZE crypto_sign_BYTES

#define S_PUBSIZE ((PUBSIZE * 5 / 4) + 1)
#define S_SECSIZE ((SECSIZE * 5 / 4) + 1)
#define S_SIGSIZE ((SIGSIZE + 2) * 4 / 3 + 1)

#define S_SIGPREFIX         crypto_sign_PRIMITIVE "-"
#define S_SIGPREFIX_LEN     (sizeof(S_SIGPREFIX) - 1)


//  Structure of our class

struct zsigcert {
    byte public_key [PUBSIZE];
    byte secret_key [SECSIZE];
    byte signature [SIGSIZE];
    char public_txt [S_PUBSIZE];
    char secret_txt [S_SECSIZE];
    char signature_txt [S_SIGPREFIX_LEN + S_SIGSIZE];
    zhash_t *metadata;
    zconfig_t *config;
    bool sodium_initialized;
};

//  --------------------------------------------------------------------------
//  Constructor

zsigcert_t *
zsigcert_new (void)
{
    byte public_key [PUBSIZE] = { 0 };
    byte secret_key [SECSIZE] = { 0 };

    if (sodium_init () < 0)
        return NULL;
    if (crypto_sign_keypair (public_key, secret_key) < 0)
        return NULL;
    zsigcert_t *self = zsigcert_new_from (public_key, secret_key);
    self->sodium_initialized = true;
    return self;
}


//  --------------------------------------------------------------------------
//  Constructor, accepts public/secret key pair from caller

zsigcert_t *
zsigcert_new_from (const byte *public_key, const byte *secret_key)
{
    zsigcert_t *self = (zsigcert_t *) zmalloc (sizeof (zsigcert_t));
    assert (self);
    assert (public_key);
    assert (secret_key);

    assert (base64_encode_length (SIGSIZE) == S_SIGSIZE);

    self->metadata = zhash_new ();
    assert (self->metadata);
    zhash_autofree (self->metadata);
    memcpy (self->public_key, public_key, PUBSIZE);
    memcpy (self->secret_key, secret_key, SECSIZE);
    zmq_z85_encode (self->public_txt, self->public_key, PUBSIZE);
    zmq_z85_encode (self->secret_txt, self->secret_key, SECSIZE);
    return self;
}


//  --------------------------------------------------------------------------
//  Destructor

void
zsigcert_destroy (zsigcert_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zsigcert_t *self = *self_p;
        zhash_destroy (&self->metadata);
        zconfig_destroy (&self->config);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Return public part of key pair as 32-byte binary string

const byte *
zsigcert_public_key (zsigcert_t *self)
{
    assert (self);
    return self->public_key;
}


//  --------------------------------------------------------------------------
//  Return secret part of key pair as 32-byte binary string

const byte *
zsigcert_secret_key (zsigcert_t *self)
{
    assert (self);
    return self->secret_key;
}


//  --------------------------------------------------------------------------
//  Return public part of key pair as Z85 armored string

const char *
zsigcert_public_txt (zsigcert_t *self)
{
    assert (self);
    return self->public_txt;
}


//  --------------------------------------------------------------------------
//  Return secret part of key pair as Z85 armored string

const char *
zsigcert_secret_txt (zsigcert_t *self)
{
    assert (self);
    return self->secret_txt;
}


//  --------------------------------------------------------------------------
//  Set certificate metadata from formatted string.

void
zsigcert_set_meta (zsigcert_t *self, const char *name, const char *format, ...)
{
    assert (self);
    assert (name);
    assert (format);

    va_list argptr;
    va_start (argptr, format);
    char *value = zsys_vprintf (format, argptr);
    va_end (argptr);
    assert (value);
    zhash_insert (self->metadata, name, value);
    zstr_free (&value);
}


//  --------------------------------------------------------------------------
//  Unset certificate metadata.

void
zsigcert_unset_meta (zsigcert_t *self, const char *name)
{
    assert (self);
    assert (name);

    zhash_delete (self->metadata, name);
}

//  --------------------------------------------------------------------------
//  Get metadata value from certificate; if the metadata value doesn't
//  exist, returns NULL.

const char *
zsigcert_meta (zsigcert_t *self, const char *name)
{
    assert (self);
    return (char *) zhash_lookup (self->metadata, name);
}


//  --------------------------------------------------------------------------
//  Get list of metadata fields from certificate. Caller is responsible for
//  destroying list. Caller should not modify the values of list items.

zlist_t *
zsigcert_meta_keys (zsigcert_t *self)
{
    assert (self);
    return zhash_keys (self->metadata);
}


//  --------------------------------------------------------------------------
//  Load certificate from file (constructor)

zsigcert_t *
zsigcert_load (const char *filename)
{
    assert (filename);

    //  Try first to load secret certificate, which has both keys
    //  Then fallback to loading public certificate
    char filename_secret [256];
    snprintf (filename_secret, 256, "%s_secret", filename);
    zconfig_t *root = zconfig_load (filename_secret);
    if (!root)
        root = zconfig_load (filename);

    zsigcert_t *self = NULL;
    if (root) {
        char *public_text = zconfig_get (root, "/curve/public-key", NULL);
        if (public_text && strlen (public_text) == S_PUBSIZE - 1) {
            byte public_key [PUBSIZE] = { 0 };
            byte secret_key [SECSIZE] = { 0 };

            char *secret_text = zconfig_get (root, "/curve/secret-key", NULL);
            zmq_z85_decode (public_key, public_text);
            if (secret_text && strlen (secret_text) == S_SECSIZE - 1)
                zmq_z85_decode (secret_key, secret_text);

            //  Load metadata into certificate
            self = zsigcert_new_from (public_key, secret_key);
            zconfig_t *metadata = zconfig_locate (root, "/metadata");
            zconfig_t *item = metadata? zconfig_child (metadata): NULL;
            while (item) {
                zsigcert_set_meta (self, zconfig_name (item), "%s", zconfig_value (item));
                item = zconfig_next (item);
            }
        }
    }
    zconfig_destroy (&root);
    return self;
}


//  --------------------------------------------------------------------------
//  Save full certificate (public + secret) to file for persistent storage
//  This creates one public file and one secret file (filename + "_secret").

static void
s_save_metadata_all (zsigcert_t *self)
{
    zconfig_destroy (&self->config);
    self->config = zconfig_new ("root", NULL);
    assert (self->config);
    zconfig_t *section = zconfig_new ("metadata", self->config);

    char *value = (char *) zhash_first (self->metadata);
    while (value) {
        zconfig_t *item = zconfig_new (zhash_cursor (self->metadata), section);
        assert (item);
        zconfig_set_value (item, "%s", value);
        value = (char *) zhash_next (self->metadata);
    }
    char *timestr = zclock_timestr ();
    zconfig_set_comment (self->config,
                         "   ****  Generated on %s by CZMQ  ****", timestr);
    zstr_free (&timestr);
}


int
zsigcert_save (zsigcert_t *self, const char *filename)
{
    assert (self);
    assert (filename);

    //  Save public certificate using specified filename
    int rc = zsigcert_save_public (self, filename);
    if (rc == -1) return rc;

    //  Now save secret certificate using filename with "_secret" suffix
    char filename_secret [256];
    snprintf (filename_secret, 256, "%s_secret", filename);
    rc = zsigcert_save_secret (self, filename_secret);
    return rc;
}

//  --------------------------------------------------------------------------
//  Save public certificate only to file for persistent storage.

int
zsigcert_save_public (zsigcert_t *self, const char *filename)
{
    assert (self);
    assert (filename);

    s_save_metadata_all (self);
    zconfig_set_comment (self->config,
                         "   ZeroMQ CURVE Public Certificate");
    zconfig_set_comment (self->config,
                         "   Exchange securely, or use a secure mechanism to verify the contents");
    zconfig_set_comment (self->config,
                         "   of this file after exchange. Store public certificates in your home");
    zconfig_set_comment (self->config,
                         "   directory, in the .curve subdirectory.");

    zconfig_put (self->config, "/curve/public-key", self->public_txt);

    return zconfig_save (self->config, filename);;
}


//  --------------------------------------------------------------------------
//  Save public certificate only to file for persistent storage.

int
zsigcert_save_secret (zsigcert_t *self, const char *filename)
{
    assert (self);
    assert (filename);

    s_save_metadata_all (self);
    zconfig_set_comment (self->config,
                         "   ZeroMQ CURVE **Secret** Certificate");
    zconfig_set_comment (self->config,
                         "   DO NOT PROVIDE THIS FILE TO OTHER USERS nor change its permissions.");
    zconfig_put (self->config, "/curve/public-key", self->public_txt);
    zconfig_put (self->config, "/curve/secret-key", self->secret_txt);

    zsys_file_mode_private ();
    int rc = zconfig_save (self->config, filename);
    zsys_file_mode_default ();
    return rc;
}


//  --------------------------------------------------------------------------
//  Return copy of certificate; if certificate is null or we exhausted
//  heap memory, returns null.

zsigcert_t *
zsigcert_dup (zsigcert_t *self)
{
    if (self) {
        zsigcert_t *copy = zsigcert_new_from (self->public_key, self->secret_key);
        if (copy) {
            zhash_destroy (&copy->metadata);
            copy->metadata = zhash_dup (self->metadata);
            if (!copy->metadata)
                zsigcert_destroy (&copy);
        }
        return copy;
    }
    else
        return NULL;
}


//  --------------------------------------------------------------------------
//  Return true if two certificates have the same keys

bool
zsigcert_eq (zsigcert_t *self, zsigcert_t *compare)
{
    assert (self);
    assert (compare);

    return (streq (self->public_txt, compare->public_txt)
         && streq (self->secret_txt, compare->secret_txt));
}


//  --------------------------------------------------------------------------
//  Print certificate contents to stdout

void
zsigcert_print (zsigcert_t *self)
{
    assert (self);
    zsys_info ("zsigcert: metadata");

    char *value = (char *) zhash_first (self->metadata);
    while (value) {
        zsys_info ("zsigcert:     %s = \"%s\"",
                   zhash_cursor (self->metadata), value);
        value = (char *) zhash_next (self->metadata);
    }
    zsys_info ("zsigcert: curve");
    zsys_info ("zsigcert:     public-key = \"%s\"", self->public_txt);
    zsys_info ("zsigcert:     secret-key = \"%s\"", self->secret_txt);
}

const char *zsigcert_sign (zsigcert_t *self, const void *buf, size_t len)
{
    assert (self);
    assert (buf);

    if (!self->sodium_initialized) {
        if (sodium_init () < 0) {
            errno = EINVAL;
            return NULL;
        }
        self->sodium_initialized = true;
    }
    if (crypto_sign_detached (self->signature, NULL,
                              buf, len, self->secret_key) < 0) {
        errno = EINVAL;
        return NULL;
    }
    int dstlen;
    strcpy (self->signature_txt, S_SIGPREFIX);
    if (base64_encode_block (self->signature_txt + S_SIGPREFIX_LEN, &dstlen,
                             self->signature, SIGSIZE) < 0) {
        errno = EINVAL;
        return NULL;
    }
    return self->signature_txt;
}

int zsigcert_verify (zsigcert_t *self, const char *s_sig,
                      const void *buf, size_t len)
{
    assert (self);
    assert (s_sig);
    assert (buf);

    if (!self->sodium_initialized) {
        if (sodium_init () < 0) {
            errno = EINVAL;
            return -1;
        }
        self->sodium_initialized = true;
    }
    if (strlen (s_sig) != S_SIGPREFIX_LEN + S_SIGSIZE - 1) {
        errno = EINVAL;
        return -1;
    }
    byte sig[SIGSIZE];
    int dstlen;
    if (base64_decode_block (sig, &dstlen, s_sig + S_SIGPREFIX_LEN,
                                           S_SIGSIZE - 1) < 0) {
        errno = EINVAL;
        return -1;
    }
    return crypto_sign_verify_detached (sig, buf, len, self->public_key);
}

int zsigcert_sign_json (zsigcert_t *self, const char *json_str,
                        char **json_signed)
{
    int rc = -1;
    const char *sig;
    int len = strlen (json_str);
    char *buf;

    /* drop any trailing whitespace plus closing bracket */
    while (len > 0 && isspace(json_str[len - 1]))
        len--;
    if (len == 0 || json_str[len - 1] != '}') {
        errno = EINVAL;
        goto done;
    }
    len--;
    /* sign truncated json */
    if (!(sig = zsigcert_sign (self, json_str, len)))
        goto done;
    /* append signature */
    if (asprintf (&buf, "%.*s,\"sig\":\"%s\"}", len, json_str, sig) < 0) {
        errno = ENOMEM;
        goto done;
    }
    *json_signed = buf;
    rc = 0;
done:
    return rc;
}

int zsigcert_verify_json (zsigcert_t *self, const char *json_str)
{
    int len = strlen (json_str);
    char sig[S_SIGPREFIX_LEN + S_SIGSIZE];

    /* drop any trailing whitespace */
    while (len > 0 && isspace(json_str[len - 1]))
        len--;
    /* isolate truncated json */
    len -= strlen (",\"sig\":\"\"}");
    len -= (S_SIGPREFIX_LEN + S_SIGSIZE - 1);
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }
    /* extract appended signature */
    const char *p = json_str + len;
    p += strlen (",\"sig\":\"");
    snprintf (sig, sizeof (sig), "%s", p);
    /* verify signature on truncated json */
    return zsigcert_verify (self, sig, json_str, len);
}

#define CHUNKSIZE 64

static int append_alloc (char **buf, int *len, int *used, char c)
{
    if (*used == *len) {
        (*len) += CHUNKSIZE;
        char *new = realloc (*buf, *len);
        if (!new) {
            errno = ENOMEM;
            return -1;
        }
        *buf = new;
    }
    (*buf)[(*used)++] = c;
    return 0;
}

static int read_json_object (FILE *f, char **s)
{
    char *buf = NULL;
    int buf_len = 0;
    int buf_used = 0;
    int brace_level = 0;
    int object_count = 0;
    int c;

    do {
        if ((c = fgetc (f)) == EOF) {
            errno = EPROTO;
            goto error;
        }
        if (append_alloc (&buf, &buf_len, &buf_used, c) < 0)
            goto error;
        switch (c) {
            case '{':
                brace_level++;
                break;
            case '}':
                if (brace_level == 0) {
                    errno = EPROTO;
                    goto error;
                }
                if (--brace_level == 0)
                    object_count++;
                break;
            default:
                break;
        }
    } while (object_count == 0);
    if (append_alloc (&buf, &buf_len, &buf_used, '\0') < 0)
        goto error;
    *s = buf;
    return 0;
error:
    free (buf);
    return -1;
}

int zsigcert_verify_json_file (zsigcert_t *self, FILE *f, char **json_str)
{
    char *s = NULL;

    if (read_json_object (f, &s) < 0)
        goto error;
    if (zsigcert_verify_json (self, s) < 0)
        goto error;
    *json_str = s;
    return 0;
error:
    free (s);
    return -1;
}

int zsigcert_sign_json_file (zsigcert_t *self, FILE *f, char **json_str)
{
    char *s = NULL;

    if (read_json_object (f, &s) < 0)
        goto error;
    if (zsigcert_sign_json (self, s, json_str) < 0)
        goto error;
    free (s);
    return 0;
error:
    free (s);
    return -1;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
