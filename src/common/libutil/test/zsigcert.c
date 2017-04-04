#include <czmq.h>
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
    diag ("Sig: %s", sig ? sig : "NULL");
    ok (sig && zsigcert_verify (cert, sig, blob, strlen (blob)) == 0,
        "zsigcert_verify works");
    char *save_sig = xstrdup (sig);

    const char blob2[] = "Nurrrmaeargasmboruggggslurrpmalwoggg";
    sig = zsigcert_sign (cert, blob2, strlen (blob2));
    ok (sig != NULL,
        "zsigcert_sign worked again");
    diag ("Blob: %s", blob2);
    diag ("Sig: %s", sig ? sig : "NULL");
    ok (sig && zsigcert_verify (cert, sig, blob2, strlen (blob2)) == 0,
        "zsigcert_verify on second blob works");

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
