#include <czmq.h>
#include <jansson.h>
#include "src/common/libtap/tap.h"
#include "src/common/libutil/xzmalloc.h"
#include "zsigcert.h"

int main (int argc, char **argv)
{
    plan (NO_PLAN);

    //  Create temporary directory for test files
#   define TESTDIR ".test_zsigcert"
    zsys_dir_create (TESTDIR);

    //  Create a simple certificate with metadata
    zsigcert_t *cert = zsigcert_new ();
    ok (cert != NULL,
        "zsigcert_new works");
    zsigcert_set_meta (cert, "email", "ph@imatix.com");
    zsigcert_set_meta (cert, "name", "Pieter Hintjens");
    zsigcert_set_meta (cert, "organization", "iMatix Corporation");
    zsigcert_set_meta (cert, "version", "%d", 1);
    ok (streq (zsigcert_meta (cert, "email"), "ph@imatix.com"),
        "zsigcert_set_meta works");
    zlist_t *keys = zsigcert_meta_keys (cert);
    ok (zlist_size (keys) == 4,
        "zlist_size works");
    zlist_destroy (&keys);

    //  Check the dup and eq methods
    zsigcert_t *shadow = zsigcert_dup (cert);
    ok (zsigcert_eq (cert, shadow),
        "zsigcert_dup works");
    zsigcert_destroy (&shadow);

    //  Check we can save and load certificate
    zsigcert_save (cert, TESTDIR "/mycert.txt");
    ok (zsys_file_exists (TESTDIR "/mycert.txt"),
        "zsigcert_save works (pub)");
    ok (zsys_file_exists (TESTDIR "/mycert.txt_secret"),
        "zsigcert_save works (secret)");

    //  Sign/verify blobs
    const char blob[] = "slerbloomorgsplormasmummmmrrrrlmm";
    const char *sig = zsigcert_sign (cert, blob, strlen (blob));
    ok (sig != NULL,
        "zsigcert_sign worked");
    diag ("Blob: %s", blob);
    diag ("Sign: %s", sig ? sig : "NULL");
    ok (sig && zsigcert_verify (cert, sig, blob, strlen (blob)) == 0,
        "zsigcert_verify works");
    char *save_sig = xstrdup (sig);

    const char blob2[] = "Nurrrmaeargasmboruggggslurrpmalwoggg";
    sig = zsigcert_sign (cert, blob2, strlen (blob2));
    ok (sig != NULL,
        "zsigcert_sign worked again");
    diag ("Blob: %s", blob2);
    diag ("Sign: %s", sig ? sig : "NULL");
    ok (sig && zsigcert_verify (cert, sig, blob2, strlen (blob2)) == 0,
        "zsigcert_verify on second blob works");

    //  Sign/verify JSON
    json_t *json = json_pack ("{s:i s:s}", "foo", 42, "bar", "mitzvah");
    if (!json)
        BAIL_OUT ("couldn't create json object");
    char *s = json_dumps (json, JSON_COMPACT);
    char *ss = NULL;
    ok (zsigcert_sign_json (cert, s, &ss) == 0 && ss != NULL,
        "zsigcert_sign_json worked");
    diag ("JSON: '%s'", s);
    diag ("Sign: '%s'", ss);

    ok (zsigcert_verify_json (cert, ss) == 0,
        "zsigcert_verify_json verified signed-json");
    ok (zsigcert_verify_json (cert, s) < 0,
        "zsigcert_verify_json failed to verify unsigned signed-json");

    char *s_tampered = xstrdup (ss);
    s_tampered[2] = 'x';
    diag ("Chng: %s", s_tampered);
    ok (zsigcert_verify_json (cert, s_tampered) < 0,
        "zsigcert_verify_json failed to verify tampered signed-json");
    free (s_tampered);

    //  Sign/verify JSON on file streams
    char path[PATH_MAX];
    int fd;
    FILE *f;
    const char *tmp = getenv ("TMPDIR");
    char *signed_s = NULL;
    char *parsed_s = NULL;
    snprintf (path, sizeof (path), "%s/zsigcert.XXXXXX", tmp ? tmp : "/tmp");
    if ((fd = mkstemp (path)) < 0)
        BAIL_OUT ("mkstemp failed");
    if (!(f = fdopen (fd, "w+")))
        BAIL_OUT ("fdopen on tmp file failed");
    if (fputs (s, f) == EOF)
        BAIL_OUT ("failed to write unsigned object to file");
    rewind (f);

    // sign and replace file
    ok (zsigcert_sign_json_file (cert, f, &signed_s) == 0 && signed_s != NULL,
        "zsigcert_sign_json_file worked");
    rewind (f);
    if (fputs (signed_s, f) == EOF)
        BAIL_OUT ("failed to write signed object to file");
    rewind (f);

    // verify file
    ok (zsigcert_verify_json_file (cert,  f, &parsed_s) == 0
        && parsed_s != NULL,
        "zsigcert_verify_json_file worked");

    diag ("Str1: '%s'", ss);
    diag ("Str2: '%s'", parsed_s);
    ok (strcmp (parsed_s, ss) == 0,
        "zsigcert_verify_json_file returned correct json object");
    ok (zsigcert_verify_json_file (cert,  f, &parsed_s) < 0,
        "zsigcert_verify_json_file failed at EOF");
    fclose (f);
    unlink (path);
    free (parsed_s);

    free (ss);
    free (s);
    json_decref (json);

    //  Make sure wrong signature fails
    ok (zsigcert_verify (cert, save_sig, blob2, strlen (blob2)) < 0,
        "zsigcert_verify first sig on second blob fails");
    ok (zsigcert_verify (cert, sig, blob, strlen (blob)) < 0,
        "zsigcert_verify second sig on first blob fails");

    free (save_sig);

    //  Load certificate, will in fact load secret one
    shadow = zsigcert_load (TESTDIR "/mycert.txt");
    if (shadow == NULL)
        BAIL_OUT ("zsigcert_load failed");
    ok (zsigcert_eq (cert, shadow),
        "zsigcert_load loaded expected content");
    zsigcert_destroy (&shadow);

    //  Delete secret certificate, load public one
    int rc = zsys_file_delete (TESTDIR "/mycert.txt_secret");
    ok (rc == 0,
        "deleted secret cert");
    shadow = zsigcert_load (TESTDIR "/mycert.txt");

    zsigcert_destroy (&shadow);
    zsigcert_destroy (&cert);

    //  Delete all test files
    zdir_t *dir = zdir_new (TESTDIR, NULL);
    assert (dir);
    zdir_remove (dir, true);
    zdir_destroy (&dir);

    done_testing ();
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
