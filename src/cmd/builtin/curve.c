/*****************************************************************************\
 *  Copyright (c) 2017 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
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
#include "builtin.h"
#include <unistd.h>
#include <inttypes.h>
#include <czmq.h>
#include <pwd.h>

#include "src/common/libutil/readall.h"
#include "src/common/libutil/zsigcert.h"

static struct optparse_option sign_opts[] =  {
    { .name = "json", .key = 'j', .has_arg = 0,
      .usage = "Sign a JSON object on stdin",
    },
    OPTPARSE_TABLE_END
};

static struct optparse_option verify_opts[] =  {
    { .name = "json", .key = 'j', .has_arg = 0,
      .usage = "Verify a JSON object on stdin",
    },
    { .name = "signature", .key = 's', .has_arg = 1,
      .usage = "Specify signature for blob on stdin",
    },
    OPTPARSE_TABLE_END
};


/* Note: flux keygen creates:
 *  $HOME/.flux/curve/signature         (contains only public key)
 *  $HOME/.flux/curve/signature_secret  (contains public, private keypair)
 */
static zsigcert_t *load_cert (uint32_t userid, int pubonly)
{
    char path[PATH_MAX];
    struct passwd *pw;
    zsigcert_t *cert;

    if (!(pw = getpwuid (userid)))
        log_msg_exit ("Could not look up userid '%"PRIu32 "' in /etc/passwd",
                      userid);
    snprintf (path, sizeof (path), "%s/.flux/curve/signature%s",
              pw->pw_dir, pubonly ? "" : "_secret");
    if (!(cert = zsigcert_load (path)))
        log_msg_exit ("failed to load %s", path);
    return cert;
}

static int internal_curve_sign (optparse_t *p, int ac, char *av[])
{
    int n;
    uint8_t *buf;
    int len;
    zsigcert_t *cert;

    n = optparse_option_index (p);
    if (n != ac) {
        optparse_print_usage (p);
        exit (1);
    }
    cert = load_cert (geteuid (), false);

    if ((len = read_all (STDIN_FILENO, &buf)) < 0)
        log_err_exit ("could not ingest stdin");

    if (optparse_hasopt (p, "json")) {
        char *json_str = xasprintf ("%.*s", len, buf);
        char *json_signed;

        if (zsigcert_sign_json (cert, json_str, &json_signed) < 0)
            log_err_exit ("could not json-sign stdin");
        printf ("%s\n", json_signed);
        free (json_signed);
        free (json_str);
    }
    else {
        const char *sig;
        if (!(sig = zsigcert_sign (cert, buf, len)))
            log_err_exit ("could not sign stdin");
        printf ("%s\n", sig);
    }

    free (buf);
    zsigcert_destroy (&cert);

    return (0);
}

static int internal_curve_verify (optparse_t *p, int ac, char *av[])
{
    int n;
    uint8_t *buf;
    int len;
    zsigcert_t *cert;
    uint32_t userid = geteuid ();

    n = optparse_option_index (p);
    if (n != ac || !(optparse_hasopt (p, "json")
                || optparse_hasopt (p, "signature"))) {
        optparse_print_usage (p);
        exit (1);
    }

    /* FIXME add option to override userid */
    cert = load_cert (userid, true);
    if ((len = read_all (STDIN_FILENO, &buf)) < 0)
        log_err_exit ("could not ingest stdin");

    if (optparse_hasopt (p, "json")) {
        char *json_str = xasprintf ("%.*s", len, buf);
        if (zsigcert_verify_json (cert, json_str) < 0)
            log_msg_exit ("verification failed");
        printf ("%s", "OK");
        free (json_str);
    }
    else if (optparse_hasopt (p, "signature")) {
        const char *sig = optparse_get_str (p, "signature", NULL);
        if (zsigcert_verify (cert, sig, buf, len) < 0)
            log_msg_exit ("verification failed");
        printf ("%s\n", "OK");
    }


    free (buf);
    zsigcert_destroy (&cert);

    return (0);
}

int cmd_curve (optparse_t *p, int ac, char *av[])
{
    log_init ("flux-curve");

    if (optparse_run_subcommand (p, ac, av) != OPTPARSE_SUCCESS)
        exit (1);
    return (0);
}

static struct optparse_subcommand curve_subcmds[] = {
    { "sign",
      "[--json]",
      "Sign stdin",
      internal_curve_sign,
      0,
      sign_opts,
    },
    { "verify",
      "[--json | --signature=SIG]",
      "Verify stdin signature",
      internal_curve_verify,
      0,
      verify_opts,
    },
    OPTPARSE_SUBCMD_END
};

int subcommand_curve_register (optparse_t *p)
{
    optparse_err_t e;

    e = optparse_reg_subcommand (p,
            "curve", cmd_curve, NULL, "Sign/verify with curve keys", 0, NULL);
    if (e != OPTPARSE_SUCCESS)
        return (-1);

    e = optparse_reg_subcommands (optparse_get_subcommand (p, "curve"),
                                  curve_subcmds);
    return (e == OPTPARSE_SUCCESS ? 0 : -1);
}

/*
 * vi: ts=4 sw=4 expandtab
 */
