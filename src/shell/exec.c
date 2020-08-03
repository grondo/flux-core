/************************************************************\
 * Copyright 2020 Lawrence Livermore National Security, LLC
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

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <flux/core.h>
#include <flux/shell.h>

#include "builtins.h"

static flux_subprocess_server_t *
shell_subprocess_server_start (flux_shell_t *shell,
                               const char *service,
                               int rank)
{
    const char *uri = getenv ("FLUX_URI");
    flux_subprocess_server_t *sp = NULL;
    flux_t *h = flux_shell_get_flux (shell);

    if (!h || !uri)
        return NULL;

    sp = flux_subprocess_server_start (h, service, uri, rank);
    if (!sp) {
        shell_log_errno ("flux_subprocess_server_start");
        return NULL;
    }
    if (flux_shell_aux_set (shell,
                            "builtin::execserver",
                            sp,
                            (flux_free_f) flux_subprocess_server_stop) < 0)
        return NULL;
    return sp;
}

static int exec_init (flux_plugin_t *p,
                      const char *topic,
                      flux_plugin_arg_t *args,
                      void *arg)
{
    const char *service;
    int shell_rank = -1;
    bool standalone = false;
    flux_shell_t *shell;
    flux_subprocess_server_t *sp = NULL;

    if (!(shell = flux_plugin_get_shell (p)))
        return shell_log_errno ("flux_plugin_get_shell");

    if (flux_shell_info_unpack (shell,
                                "{s:i s:s s:{s:b}}",
                                "rank", &shell_rank,
                                "service", &service,
                                "options", "standalone", &standalone) < 0)
        return shell_log_errno ("flux_shell_info_unpack: service");

    /* Do not enable subprocess server in standlone mode */
    if (standalone)
        return 0;

    if (!(sp = shell_subprocess_server_start (shell, service, shell_rank)))
        return -1;

    shell_debug ("started exec service at %s.rexec", service);

    return 0;
}

struct shell_builtin builtin_exec = {
    .name = "exec",
    .init = exec_init,
};

/* vi: ts=4 sw=4 expandtab
 */
