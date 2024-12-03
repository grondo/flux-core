#############################################################
# Copyright 2024 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################

import argparse
import json
import sys
from collections import deque
from pathlib import Path

import flux
import flux.kvs
from flux.core.inner import raw
from flux.modprobe import Modprobe
from flux.util import CLIMain, Tree, help_formatter


def conf_builtin_get(name):
    pythonpath = Path(
        raw.flux_conf_builtin_get("python_path", raw.FLUX_CONF_INSTALLED).decode(
            "utf-8"
        )
    ).resolve()
    flux_modpath = Path(flux.__file__).resolve()
    if pythonpath in flux_modpath.parents:
        flag = raw.FLUX_CONF_INSTALLED
    else:
        flag = raw.FLUX_CONF_INTREE
    return raw.flux_conf_builtin_get(name, flag).decode("utf-8")


def default_flux_confdir():
    """Derive sysconfdir from builtin rc1_path"""
    return Path(conf_builtin_get("rc1_path")).parent


def runtasks(args, timing=False, rcfile="rc1"):
    M = Modprobe(confdir=args.confdir, timing=timing)
    t0 = M.timestamp
    M.configure_modules()
    M.read_rcfile(rcfile)
    M.add_timing("configure", t0)
    deps = M.get_deps(M.solve(M.active_tasks))
    if args.show_deps:
        print(json.dumps(deps))
        sys.exit(0)
    M.run(deps)
    if timing and M.rank == 0:
        flux.kvs.put(M.context.handle, "modprobe.stats", M.timing)
        flux.kvs.commit(M.context.handle)
    sys.exit(M.exitcode)


def run(args):
    args.confdir = default_flux_confdir()
    runtasks(args, rcfile=args.file)


def rc1(args):
    runtasks(args, timing=args.timing)


def rc3(args):
    M = Modprobe(confdir=args.confdir)
    M.configure_modules()
    # rc3 always removes all modules
    M.set_remove()
    M.read_rcfile("rc3")
    deps = M.get_deps(M.active_tasks)
    M.run(deps)
    sys.exit(M.exitcode)


def load(args):
    M = Modprobe()
    M.configure_modules("etc/modules.toml")
    M.load(args.modules)
    sys.exit(M.exitcode)


def remove(args):
    M = Modprobe()
    M.configure_modules("etc/modules.toml")
    M.remove(args.modules)
    sys.exit(M.exitcode)


def list_dependencies(args):
    M = Modprobe()
    M.configure_modules("etc/modules.toml")

    queue = deque()
    visited = set()

    task = M.get_task(args.module)
    label = args.module
    if task.name != label:
        label += f" ({task.name})"

    parent = Tree(args.module)
    queue.append(parent)

    while queue:
        tree = queue.popleft()
        task = M.get_task(tree.label)
        if args.full and task.name in visited:
            continue

        for dep in task.requires:
            if dep not in visited:
                child = Tree(dep)
                queue.append(child)
                tree.append_tree(child)
                if not args.full:
                    visited.add(dep)
        visited.add(task.name)
    parent.label = label
    parent.render()


def list_after(args):
    M = Modprobe()
    M.configure_modules()
    M.get_depM.solve(args.module)


def parse_args():
    parser = argparse.ArgumentParser(
        prog="flux-modprobe",
        description="Task and module load/unload management for Flux",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(
        title="subcommands", description="", dest="subcommand"
    )
    subparsers.required = True

    load_parser = subparsers.add_parser("load", formatter_class=help_formatter())
    load_parser.add_argument(
        "modules",
        metavar="MODULE",
        nargs="+",
        help="Module to load",
    )
    load_parser.set_defaults(func=load)

    remove_parser = subparsers.add_parser("remove", formatter_class=help_formatter())
    remove_parser.add_argument(
        "modules",
        metavar="MODULE",
        nargs="+",
        help="Module to remove",
    )
    remove_parser.set_defaults(func=remove)

    run_parser = subparsers.add_parser(
        "run",
        formatter_class=help_formatter(),
    )
    run_parser.add_argument(
        "--show-deps",
        action="store_true",
        help="Display dictionary of tasks to predecessor list and exit",
    )
    run_parser.add_argument(
        "file", metavar="FILE", help="Run commands defined in Python rc file FILE"
    )
    run_parser.set_defaults(func=run)

    rc1_parser = subparsers.add_parser("rc1", formatter_class=help_formatter())
    rc1_parser.add_argument(
        "--confdir",
        metavar="DIR",
        default=default_flux_confdir(),
        help="Set default config directory",
    )
    rc1_parser.add_argument(
        "--timing",
        action="store_true",
        help="Store timing data in KVS in modprobe.stats key",
    )
    rc1_parser.add_argument(
        "--show-deps",
        action="store_true",
        help="Display dictionary of tasks to predecessor list and exit",
    )
    rc1_parser.set_defaults(func=rc1)

    rc3_parser = subparsers.add_parser("rc3", formatter_class=help_formatter())
    rc3_parser.add_argument(
        "--confdir",
        metavar="DIR",
        default=default_flux_confdir(),
        help="Set default config directory",
    )
    rc3_parser.set_defaults(func=rc3)

    list_deps_parser = subparsers.add_parser(
        "list-dependencies", formatter_class=help_formatter()
    )
    list_deps_parser.add_argument(
        "--full",
        "-f",
        action="store_true",
        help="Full output. Will show duplicates",
    )
    list_deps_parser.add_argument(
        "module",
        metavar="MODULE",
        type=str,
        help="Module name for which to display dependencies",
    )
    list_deps_parser.set_defaults(func=list_dependencies)

    return parser.parse_args()


@CLIMain()
def main():
    args = parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
