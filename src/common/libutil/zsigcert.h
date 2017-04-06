/* derived from czmq zert.h */

#ifndef __ZSIGCERT_H_INCLUDED__
#define __ZSIGCERT_H_INCLUDED__

#include <czmq.h>

typedef struct zsigcert zsigcert_t;

zsigcert_t *zsigcert_new (void);

zsigcert_t *zsigcert_new_from (const byte *public_key,
		                 const byte *secret_key);

zsigcert_t *zsigcert_load (const char *filename);

void zsigcert_destroy (zsigcert_t **self_p);

const byte *zsigcert_public_key (zsigcert_t *self);

const byte *zsigcert_secret_key (zsigcert_t *self);

const char *zsigcert_public_txt (zsigcert_t *self);

const char *zsigcert_secret_txt (zsigcert_t *self);

void zsigcert_set_meta (zsigcert_t *self, const char *name,
		         const char *format, ...) CHECK_PRINTF (3);

const char *zsigcert_meta (zsigcert_t *self, const char *name);

zlist_t *zsigcert_meta_keys (zsigcert_t *self);

int zsigcert_save (zsigcert_t *self, const char *filename);

int zsigcert_save_public (zsigcert_t *self, const char *filename);

int zsigcert_save_secret (zsigcert_t *self, const char *filename);

zsigcert_t *zsigcert_dup (zsigcert_t *self);

bool zsigcert_eq (zsigcert_t *self, zsigcert_t *compare);

void zsigcert_print (zsigcert_t *self);


/* Signatures are of the form:
 * <algorithm>-<base64 string>
 *
 * Example:
 * ed25519-7sfhxyEuDSQFuVsoUnwp5vONvBYpLfIwhkRHGTFgiviUN5abVyVcWTfqauA2rsgHBnFuUVb8VDqtOmTuIf6nCw==
 */

/* Sign (buf, len) returning signature.
 * Cert must contain private key.
 * Signature is valid until next call.
 */
const char *zsigcert_sign (zsigcert_t *self, const void *buf, size_t len);

/* Verify buf with signature
 * Cert must contain public key.
 */
int zsigcert_verify (zsigcert_t *self, const char *sig,
                      const void *buf, size_t len);

/* Valid JSON in, valid JSON out with signature (caller must free).
 * See https://camlistore.org/doc/json-signing/
 */
int zsigcert_sign_json (zsigcert_t *self, const char *json_str,
                        char **json_signed);

int zsigcert_verify_json (zsigcert_t *self, const char *json_str);

/* Verify JSON object on 'f', returning it in json_str (caller must free).
 * Position 'f' at the byte following the json object.
 */
int zsigcert_verify_json_file (zsigcert_t *self, FILE *f, char **json_str);

/* Sign JSON object on 'f', returning it in json_str (caller must free).
 * Position 'f' at the byte following the json object.
 */
int zsigcert_sign_json_file (zsigcert_t *self, FILE *f, char **json_str);

#endif
