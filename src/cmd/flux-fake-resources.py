###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Inject synthetic resources into the current Flux instance.

A standalone command-line wrapper for
:class:`flux.testing.fake_resources.InjectFakeResources`. Useful
for prototyping, debugging, and as a setup step in shell-driven
integration tests.

The numeric ``--cores`` and ``--gpus`` flags describe a flat
resource shape. For on-node topology (sockets, NUMA), supply real
hwloc XML via ``--hwloc-xml=PATH``; ``flux R encode --local`` will
consume it. For scheduler-specific R metadata (e.g. fluxion JGF
keys), supply a Python plugin via ``--amend-r=PATH``.
"""

import argparse
import logging
import os

import flux
import flux.importer
import flux.util
from flux.testing.fake_resources import InjectFakeResources

LOGGER = logging.getLogger("flux-fake-resources")


class _CLIFakeResources(InjectFakeResources):
    """InjectFakeResources subclass that delegates amend_R to a plugin."""

    def __init__(self, *, amender_fn=None, **kw):
        super().__init__(**kw)
        self._amender_fn = amender_fn

    def amend_R(self, R, hwloc_xml=None):
        if self._amender_fn is None:
            return R
        return self._amender_fn(R, self, hwloc_xml=hwloc_xml)


def _load_amender(path):
    """Load a Python file with a top-level ``amend()`` function."""
    if not os.path.isfile(path):
        raise FileNotFoundError(f"--amend-r: no such file: {path}")
    module = flux.importer.import_path(path)
    if not hasattr(module, "amend"):
        raise ValueError(
            f"{path}: missing top-level amend(R, fake_resources, "
            f"hwloc_xml=None) function"
        )
    return module.amend


def parse_args():
    parser = argparse.ArgumentParser(
        prog="flux-fake-resources",
        formatter_class=flux.util.help_formatter(),
        description=("Inject fake resources into the current Flux instance"),
    )
    parser.add_argument(
        "-N",
        "--nnodes",
        type=int,
        required=True,
        metavar="N",
        help="number of fake nodes to inject",
    )
    parser.add_argument(
        "-c",
        "--cores",
        dest="cores_per_node",
        type=int,
        default=1,
        metavar="N",
        help="cores per node (default: 1)",
    )
    parser.add_argument(
        "-g",
        "--gpus",
        dest="gpus_per_node",
        type=int,
        default=0,
        metavar="N",
        help="GPUs per node (default: 0)",
    )
    parser.add_argument(
        "-H",
        "--host-prefix",
        default="fake",
        metavar="PREFIX",
        help="hostname prefix for synthetic nodes (default: fake)",
    )
    parser.add_argument(
        "--scheduler",
        default="sched-simple",
        metavar="MODULE",
        help="scheduler module to load (default: sched-simple)",
    )
    parser.add_argument(
        "--scheduler-options",
        metavar="OPTS",
        help=("module options string for the scheduler, parsed via shlex"),
    )
    parser.add_argument(
        "--hwloc-xml",
        metavar="PATH",
        help=(
            "generate R from hwloc XML at PATH instead of from "
            "--cores/--gpus. XML will be passed to amend()."
        ),
    )
    parser.add_argument(
        "--amend-r",
        metavar="PATH",
        help=(
            "Python file with top-level amend(R, fake_resources, "
            "hwloc_xml=None) function to mutate R after generation"
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="trace each subprocess and RPC step on stderr",
    )
    return parser.parse_args()


@flux.util.CLIMain(LOGGER)
def main():
    args = parse_args()
    amender_fn = _load_amender(args.amend_r) if args.amend_r else None

    fr = _CLIFakeResources(
        nodes=args.nnodes,
        cores_per_node=args.cores_per_node,
        gpus_per_node=args.gpus_per_node,
        host_prefix=args.host_prefix,
        hwloc_xml_path=args.hwloc_xml,
        verbose=args.verbose,
        amender_fn=amender_fn,
    )
    fr.install(
        flux.Flux(),
        scheduler=args.scheduler,
        scheduler_options=args.scheduler_options,
    )


if __name__ == "__main__":
    main()

# vi: ts=4 sw=4 expandtab
