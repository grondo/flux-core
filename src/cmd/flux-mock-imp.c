/*****************************************************************************
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

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <envz.h>
#include <argz.h>
#include <jansson.h>
#include <flux/optparse.h>

#include "src/common/libutil/log.h"
#include "src/common/libutil/zsigcert.h"

struct pid_info {
    pid_t pid;
    char cg_path [PATH_MAX];
    uid_t cg_owner;
};

int cmd_version (optparse_t *p, int argc, char **argv);
int cmd_run (optparse_t *p, int argc, char **argv);
int cmd_kill (optparse_t *p, int argc, char **argv);

static struct optparse_subcommand subcommands[] = {
    { "version",
      NULL,
      "Print IMP version to stdout.",
      cmd_version,
      0,
      NULL
    },
    { "run",
      NULL,
      "Run command described by signed credential on stdin",
      cmd_run,
      0,
      NULL
    },
    { "kill",
      "PID",
      "Send SIGKILL to PID",
      cmd_kill,
      0,
      NULL
    },
    OPTPARSE_SUBCMD_END
};

int usage (optparse_t *p, struct optparse_option *o, const char *optarg)
{
    struct optparse_subcommand *s;
    optparse_print_usage (p);
    fprintf (stderr, "\n");
    fprintf (stderr, "IMP Subcommands:\n");
    s = subcommands;
    while (s->name) {
        fprintf (stderr, "   %-15s %s\n", s->name, s->doc);
        s++;
    }
    exit (1);
}

int main (int argc, char *argv[])
{
    char *cmdusage = "[OPTIONS] COMMAND ARGS";
    optparse_t *p;
    int optindex;
    int exitval;

    log_init ("flux-mock-imp");

    p = optparse_create ("flux-mock-imp");

    /* Override help option for our own */
    if (optparse_set (p, OPTPARSE_USAGE, cmdusage) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_set (USAGE)");

    /* Override --help callback in favor of our own above */
    if (optparse_set (p, OPTPARSE_OPTION_CB, "help", usage) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_set() failed");

    /* Don't print internal subcommands, we do it ourselves */
    if (optparse_set (p, OPTPARSE_PRINT_SUBCMDS, 0) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_set (PRINT_SUBCMDS)");

    if (optparse_reg_subcommands (p, subcommands) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_reg_subcommands");

    if ((optindex = optparse_parse_args (p, argc, argv)) < 0)
        exit (1);

    if ((argc - optindex == 0) || !optparse_get_subcommand (p, argv[optindex])) {
        usage (p, NULL, NULL);
        exit (1);
    }

    if ((exitval = optparse_run_subcommand (p, argc, argv)) < 0)
        exit (1);

    optparse_destroy (p);
    log_fini ();
    return (exitval);
}

int cmd_version (optparse_t *p, int argc, char **argv)
{
    if (argc != 1)
        log_msg_exit ("version subcommand accepts no arguments");
    printf ("%s-%s\n", PACKAGE_NAME, PACKAGE_VERSION);
    return (0);
}

/*
 *  Permanently drop any privileges and become user described by
 *   struct passwd entry `pw`.
 */
static int switch_user (struct passwd *pw)
{
    /* Initialize groups from /etc/group */
    if (initgroups (pw->pw_name, pw->pw_gid) < 0)
        log_msg_exit ("initgroups");

    /* Set saved, effective, and real gid/uids */
    if (setresgid (pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0)
        log_msg_exit ("setregid");
    if (setresuid (pw->pw_uid, pw->pw_uid, pw->pw_uid) < 0)
        log_msg_exit ("setreuid");

    /* Verify we can't restore privilege */
    if (setreuid (-1, 0) == 0)
        log_msg_exit ("drop privileges failed");

    return (0);
}

static char **expand_argz (char *argz, size_t argz_len)
{
    size_t len;
    char **argv;
    len = argz_count (argz, argz_len) + 1;
    argv = malloc (len * sizeof (char *));
    argz_extract (argz, argz_len, argv);
    return (argv);
}

/*
 *  Return array in `req` under key `name` as an argz object.
 */
static int json_get_argz (json_t *req, const char *name,
    char **argzp, size_t *argz_lenp)
{
    json_t *o, *value;
    size_t index;
    char *argz = NULL;
    size_t argz_len = 0;

    *argzp = NULL;
    *argz_lenp = 0;

    if (!(o = json_object_get (req, name)))
        return (-1);

    if (!json_is_array (o))
        return (-1);

    json_array_foreach (o, index, value) {
        const char *arg = json_string_value (value);
        if ((arg == NULL) || (argz_add (&argz, &argz_len, arg) != 0))
            goto err;
    }
    *argzp = argz;
    *argz_lenp = argz_len;
    return (0);
err:
    free (argz);
    return (-1);
}


/*
 *  Return dictionary in `req` under key `name` as an envz object.
 */
static int json_get_envz (json_t *req, const char *name,
    char **envzp, size_t *envz_lenp)
{
    json_t *o, *value;
    const char *key = NULL;
    char *envz = NULL;
    size_t envz_len = 0;

    *envzp = NULL;
    *envz_lenp = 0;

    if (!(o = json_object_get (req, name)))
        return (-1);

    json_object_foreach (o, key, value) {
        const char *val = json_string_value (value);
        if ((val == NULL) || (envz_add (&envz, &envz_len, key, val) != 0))
            goto err;
    }

    *envzp = envz;
    *envz_lenp = envz_len;
    return (0);
err:
    free (envz);
    return (-1);
}

/*
 *  Exec cmdline described in `req` as UID `uid`.
 */
static int exec_request_as_user (json_t *req, uid_t uid)
{
    struct passwd *pw;
    json_t *o;
    const char *cwd = NULL;
    char *argz = NULL;
    size_t argz_len = 0;
    char *envz = NULL;
    size_t envz_len = 0;
    char **argv = NULL;
    char **env = NULL;

    /* Move to safe path */
    if (chdir ("/") < 0)
        log_msg_exit ("chdir ('/')");

    if (!(pw = getpwuid (uid)))
        log_msg_exit ("no passwd entry for UID %ju", (uintmax_t) uid);

    if ((getuid () != uid) && switch_user (pw) < 0)
        log_msg_exit ("failed to switch to UID %ju\n", (uintmax_t) uid);

    if (!(o = json_object_get (req, "cwd")) || !(cwd = json_string_value (o)))
        cwd = pw->pw_dir;

    if (chdir (cwd) < 0)
        log_msg_exit ("chdir (%s)", cwd);

    //log_msg ("chdir ('%s')", cwd);

    if (json_get_argz (req, "cmdline", &argz, &argz_len) < 0)
        log_msg_exit ("failed to get cmdline from request");

    if (json_get_envz (req, "env", &envz, &envz_len) < 0)
        log_msg_exit ("failed to get cmdline from request");

    /* Propagate FLUX_URI for child process */
    if (envz_add (&envz, &envz_len, "FLUX_URI", getenv ("FLUX_URI")) != 0)
        log_msg_exit ("Out of memory adding FLUX_URI to environment");

    envz_strip (&envz, &envz_len);

    argv = expand_argz (argz, argz_len);
    env = expand_argz (envz, envz_len);

    //log_msg ("calling execvp(%s)", argv[0]);
    execvpe (argv[0], argv, env);
    fprintf (stderr, "%s: %s", argv[0], strerror (errno));
    if (errno == EPERM || errno == EACCES)
        exit (126);
    exit (127);
}


/*
 *  Load cert for userid. Lifted from zsigcert.c
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

struct cmd_request {
    uid_t euid;
    uid_t guest;
    uid_t owner;
    json_t *J;
    json_t *R;
};

/*
 *  Get "userid" member of JSON object `o`, stored in `uidp`.
 */
static int get_userid (json_t *o, uid_t *uidp)
{
    json_int_t uid;
    json_t *userid;

    if (uidp == NULL)
        return (-1);

    if (!(userid = json_object_get (o, "userid"))) {
        log_msg ("`userid` not found in JSON input");
        return (-1);
    }

    if ((uid = json_integer_value (userid)) < 0) {
        log_err ("Invalid `userid` in JSON input");
        return (-1);
    }

    *uidp = (uid_t) uid;

    return (0);
}

/*
 *  Verify and parse JSON object on file `fp` with zsigcert `zs`.
 *
 *  Returns parsed json_t object or NULL on error with error structure `ep`
 *   filled out.
 */
static json_t * zs_verify_loadf (zsigcert_t *zs, FILE *fp, json_error_t *ep)
{
    char *json_str;
    json_t *ret = NULL;
    if (zsigcert_verify_json_file (zs, fp, &json_str) < 0) {
        if (ep) {
            ep->line = 0;
            ep->column = 0;
            strncpy (ep->text, "JSON signature verification failed",
                     JSON_ERROR_TEXT_LENGTH);
        }
        return (NULL);
    }
    ret = json_loads (json_str, 0, ep);

    free (json_str);
    return (ret);
}

/*
 *  Read and verify signed JSON blob on file `fp` for UID uid.
 */
static json_t *zcert_json_verify_loadf (uid_t uid, FILE *fp)
{
    json_t *ret;
    json_error_t e;
    zsigcert_t *zs = load_cert (uid, 1);

    if (zs == NULL) {
        log_err ("Failed to load public cert for UID %ju", (uintmax_t) uid);
        return (NULL);
    }

    if (!(ret = zs_verify_loadf (zs, fp, &e)))
        log_err ("JSON input: %d:%d: %s", e.line, e.column, e.text);
    zsigcert_destroy (&zs);

    return (ret);
}


/*
 *  Placeholder validation check. Ensure that R.J matches J.sig.
 */
static int cmd_request_validate (struct cmd_request *cmd)
{
    json_t *o;
    const char *sig = NULL;
    const char *sigR = NULL;

    if (!(o = json_object_get (cmd->R, "J"))
        || !(sigR = json_string_value (o))) {
        log_err ("Unable to get field J from R");
        return (-1);
    }
    if (!(o = json_object_get (cmd->J, "sig"))
        || !(sig = json_string_value (o))) {
        return (-1);
    }
    if (strcmp (sigR, sig) != 0)
        return (-1);
    return (0);
}

static int cmd_initialize_credentials (struct cmd_request *cmd)
{
    uid_t ruid, euid, suid;

    if (getresuid (&ruid, &euid, &suid) < 0)
        return (-1);

    cmd->owner = ruid;
    cmd->euid = euid;
    cmd->guest = (uid_t) -1;

    return (0);
}

/*
 *  Exec a command, described on stdin by two JSON objects: R, J.
 *
 *  If the current process is running with EUID == 0, then run
 *  command as userid specified in R, but only if J was signed by
 *  this user.
 *
 *  J must immediately follow R, separated only by whitespace.
 *
 *  Rough content rules for R, J:
 *
 *  # jcr-version 0.6
 *  # R:
 *  {
 *     "userid": 1..,
 *     "J":      string,
 *     signature,
 *  }
 *
 *  # J:
 *  {
 *     cmdline,
 *     environment,
 *     ?cwd,
 *     signature,
 *  }
 *
 *  cmdline "cmdline" [ *: string ]
 *  environment "env" { *: env_var ]
 *  env_var { /.* /: any }
 *  cwd "cwd" string
 *  signature "sig" string
 */
int cmd_run (optparse_t *p, int argc, char **argv)
{
    struct cmd_request cmd;
    memset (&cmd, 0, sizeof (cmd));

    if (cmd_initialize_credentials (&cmd) < 0)
        log_msg_exit ("failed to get current process credentials");

    //log_msg ("euid = %ju, owner (real uid) is %ju",
    //    (uintmax_t) cmd.euid, (uintmax_t) cmd.owner);

    if (cmd.euid != 0)
        log_msg ("Running in unprivileged mode...");

    if (!(cmd.R = zcert_json_verify_loadf (cmd.owner, stdin)))
        log_msg_exit ("Unable to process owner request on stdin");

    if (get_userid (cmd.R, &cmd.guest) < 0)
        log_msg_exit ("No userid member in JSON input");

    if (cmd.guest == 0)
        log_msg_exit ("Cowardly refusing to run with with guest UID 0");

    //log_msg ("Executing as guest UID %ju", (uintmax_t) cmd.guest);

    if (!(cmd.J = zcert_json_verify_loadf (cmd.guest, stdin)))
        log_msg_exit ("Unable to process guest request on stdin");

    //log_msg ("Verified authenticity of req from %ju", (uintmax_t) cmd.guest);

    if (cmd_request_validate (&cmd) < 0)
        log_msg_exit ("invalid R for J");

    exec_request_as_user (cmd.J, cmd.guest);
    /* NORETURN */
    return (-1);
}


static int pid_info_load (struct pid_info *p, pid_t pid);

static int check_and_kill_process (struct cmd_request *cmd, pid_t pid)
{
    struct pid_info pi;

    if (pid_info_load (&pi, pid) < 0)
        log_msg_exit ("Failed to get info for pid %ju\n", (uintmax_t) pid);

    /* Check if pid is in pids cgroup owned by owner UID */
    if (pi.cg_owner != cmd->owner)
        log_msg_exit ("Refusing kill request from UID %ju to process %jd",
                      (uintmax_t) cmd->owner, (intmax_t) pid);
    /* Send signal */
    if (kill (pid, SIGKILL) < 0) {
        int code = errno;
        log_err ("kill");
        exit (code);
    }
    return (0);
}

int cmd_kill (optparse_t *p, int argc, char **argv)
{
    struct cmd_request cmd;
    int i;

    if (argc < 1)
        log_msg_exit ("Missing PID argument");

    memset (&cmd, 0, sizeof (cmd));

    if (cmd_initialize_credentials (&cmd) < 0)
        log_msg_exit ("failed to get current process credentials");

    if (cmd.euid != 0)
        log_msg ("Running in unprivileged mode...");

    for (i = optparse_option_index (p); i < argc; i++) {
        char *p;
        long pid = strtol (argv[i], &p, 10);
        /*  As in kill (2), pid may be < 0 */
        if (*p != '\0' || pid == 0)
            log_msg_exit ("Invalid PIDs argument '%s'\n", argv [i]);
        check_and_kill_process (&cmd, pid);
    }
    return (0);
}

static const char cgroup_mount_dir[] = "/sys/fs/cgroup/systemd";

static int pid_systemd_cgroup_path (pid_t pid, char *buf, size_t len)
{
    int rc = -1;
    FILE *fp;
    size_t size = 0;
    ssize_t n;
    char file [PATH_MAX];
    char *line = NULL;

    n = snprintf (file, sizeof(file), "/proc/%ju/cgroup", (uintmax_t) pid);
    if ((n < 0) || (n >= PATH_MAX))
        return (-1);

    if (!(fp = fopen (file, "r")))
        return (-1);

    while ((n = getline (&line, &size, fp)) >= 0) {
        char *nl;
        char *relpath = NULL;
        char *subsys = strchr (line, ':');
        if ((nl = strchr (line, '\n')))
            *nl = '\0';
        if (subsys == NULL || *(++subsys) == '\0'
            || !(relpath = strchr (subsys, ':')))
            continue;
        /* Nullify subsys, relpath is already nul-terminated at newline */
        *(relpath++) = '\0';
        if (strcmp (subsys, "name=systemd") == 0) {
            n = snprintf (buf, len, "%s%s", cgroup_mount_dir, relpath);
            if ((n > 0) && (n < len))
                rc = 0;
            break;
        }
    }

    free (line);
    fclose (fp);
    return (rc);
}

static uid_t path_owner (const char *path)
{
    struct stat st;
    if (stat (path, &st) < 0) {
        log_err ("stat (%s)", path);
        return ((uid_t) -1);
    }
    return st.st_uid;
}

static int pid_info_load (struct pid_info *p, pid_t pid)
{
    if (pid < 0)
        pid = -pid;
    p->pid = pid;
    if (pid_systemd_cgroup_path (pid, p->cg_path, sizeof (p->cg_path)) < 0)
        return (-1);
    if ((p->cg_owner = path_owner (p->cg_path)) == (uid_t) -1)
        return (-1);
    // ENOTSUPPORTED p->uid = pid_owner (pid);
    return (0);
}


/*
 * vi: tabstop=4 shiftwidth=4 expandtab
 */
