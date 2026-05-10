###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Fake resource generation and installation for testing.

Provides a :class:`FakeResources` ABC describing a synthetic
resource set with a flat shape (nodes × cores × gpus), plus an
:class:`InjectFakeResources` implementation that mutates a running
broker by encoding R via ``flux R encode``, writing it to the KVS,
and reloading the resource module so the new R takes effect.
Because the scheduler module depends on the resource module,
:meth:`InjectFakeResources.install` also unloads the currently
loaded scheduler before the resource module is reloaded and
re-loads the requested scheduler afterward.

Also exports :func:`reload_scheduler`, a helper that unloads the
currently-loaded scheduler module (if any) and loads a named one
in its place.
"""

import abc
import json
import os
import shlex
import subprocess
import sys

from flux.idset import IDset
from flux.modprobe import ModuleList


def _stderr_log(msg):
    """Default log function: print ``msg`` to stderr.

    The CLI overrides this with ``LOGGER.info`` (or similar) for
    prefixed output; tests pass a no-op to suppress noise.
    """
    print(msg, file=sys.stderr)


class FakeResources(abc.ABC):
    """Abstract base for synthetic resource sets with a flat shape.

    A resource set described as a count of nodes, cores per node,
    and (optionally) GPUs per node. On-node topology (sockets,
    NUMA domains) is not modeled here; subclasses or callers
    needing topology should pass real hwloc XML, which
    ``flux R encode --local`` will consume, or override
    :meth:`amend_R` to inject scheduler-specific metadata.

    Subclasses must implement :meth:`install`, which makes the
    resource set visible to the broker. Subclasses may also
    override :meth:`amend_R` to mutate the encoded R before it is
    installed.
    """

    def __init__(self, nodes, cores_per_node=1, gpus_per_node=0, host_prefix="fake"):
        self.nodes = nodes
        self.cores_per_node = cores_per_node
        self.gpus_per_node = gpus_per_node
        self.host_prefix = host_prefix

    @property
    def total_cores(self):
        """Total core count across all nodes."""
        return self.nodes * self.cores_per_node

    @property
    def total_gpus(self):
        """Total GPU count across all nodes."""
        return self.nodes * self.gpus_per_node

    @abc.abstractmethod
    def install(self):
        """Make these resources visible to the broker.

        Concrete subclasses may extend this signature with
        additional arguments.
        """

    def saturation_count(self, slot_cores=1, slot_gpus=0):
        """Number of slot-shape jobs needed to saturate this resource set.

        See :func:`saturation_count` for the algorithm. This method
        is a thin wrapper that pulls ``nodes``, ``cores_per_node``,
        and ``gpus_per_node`` from ``self``; it exists so callers
        holding a FakeResources object don't have to unpack its
        attributes. The function form is what
        ``flux schedbench`` calls for the real-resource path,
        where there's no FakeResources object — the shape comes
        from a ``flux.resource.resource_list`` query.
        """
        return saturation_count(
            self.nodes,
            self.cores_per_node,
            self.gpus_per_node,
            slot_cores=slot_cores,
            slot_gpus=slot_gpus,
        )

    def amend_R(self, R, hwloc_xml=None):
        """Hook to mutate R after generation, before KVS install.

        Override in a subclass to inject scheduler-specific
        metadata (e.g. fluxion JGF keys) into R. The default
        implementation is a no-op. When called against an XML-
        derived R, ``hwloc_xml`` is the loaded XML string;
        otherwise it is None.
        """
        return R


def saturation_count(nodes, cores_per_node, gpus_per_node, slot_cores=1, slot_gpus=0):
    """Return the number of slot-shape jobs that saturate a flat
    resource set.

    A ``slot`` is described by its core and GPU counts; this
    returns the number of such slots that fit across ``nodes``
    machines of ``cores_per_node`` cores and ``gpus_per_node``
    GPUs each, taking the more-constraining of cores and GPUs
    into account. Pure arithmetic — no broker access, no
    FakeResources object required — so it's reusable from
    code paths that gather the shape from
    :func:`flux.resource.resource_list` instead.

    Either ``slot_cores`` or ``slot_gpus`` must be positive.

    Non-uniformity note: this assumes uniform cores/GPUs per
    node. Real broker resources may not be uniform; callers
    that derive the shape from a broker query typically average
    across nodes, which can over- or under-estimate true
    capacity by one slot per heterogeneous node. The error is
    bounded and not a correctness issue for benchmarks that
    measure aggregate rates.
    """
    if slot_cores < 1 and slot_gpus < 1:
        raise ValueError("slot_cores or slot_gpus must be positive")
    per_node = cores_per_node // max(slot_cores, 1)
    if slot_gpus:
        per_node = min(per_node, gpus_per_node // slot_gpus)
    return nodes * per_node


def build_R_encode_args(fr):
    """Build argv for ``flux R encode`` from a :class:`FakeResources`.

    Exposed separately from :class:`InjectFakeResources` so unit
    tests can verify command construction without requiring a
    broker.

    Idset-shaped arguments (``-r``, ``-c``, ``-g``) are formatted
    via :class:`flux.idset.IDset` rather than ``f"0-{n-1}"``. A
    degenerate single-element range like ``"0-0"`` is rejected
    when ``flux R encode`` parses it as an idset, which silently
    produced R without the affected resource type (the
    ``gpus_per_node=1`` case originally surfaced this).
    :class:`IDset` emits ``"0"`` for a one-element set, which
    parses cleanly.
    """
    n = fr.nodes
    ranks = IDset()
    ranks.set(0, n - 1)
    cores = IDset()
    cores.set(0, fr.cores_per_node - 1)
    cmd = [
        "flux",
        "R",
        "encode",
        "-r",
        str(ranks),
        "-H",
        f"{fr.host_prefix}[0-{n - 1}]",
        "-c",
        str(cores),
    ]
    if fr.gpus_per_node > 0:
        gpus = IDset()
        gpus.set(0, fr.gpus_per_node - 1)
        cmd += ["-g", str(gpus)]
    return cmd


class InjectFakeResources(FakeResources):
    """:class:`FakeResources` that mutates a running broker.

    :meth:`install` performs the four-step sequence required to
    swap in fake resources without leaving the scheduler talking
    to a half-loaded resource module:

      1. Write R to ``resource.R`` in the KVS.
      2. Unload the currently loaded scheduler (if any).
      3. Reload the resource module with ``noverify`` and
         ``monitor-force-up`` so the fake R is accepted regardless
         of actual hardware and so all ranks come up "up".
      4. Load the requested scheduler with the given options.

    If ``hwloc_xml_path`` is set, R is encoded by passing the
    supplied XML to ``flux R encode --local`` via the
    ``HWLOC_XMLFILE`` environment variable rather than from the
    numeric ``cores_per_node`` / ``gpus_per_node`` fields. The
    XML's loaded contents are passed to :meth:`amend_R` so
    subclasses can use them.

    The ``log`` argument is a callable taking a single string
    (default :func:`_stderr_log`). Pass ``LOGGER.info`` to
    integrate with a CLI's logging configuration, or
    ``lambda _: None`` to suppress output entirely.

    Must be invoked from within a Flux instance.
    """

    def __init__(
        self,
        nodes,
        cores_per_node=1,
        gpus_per_node=0,
        host_prefix="fake",
        hwloc_xml_path=None,
        verbose=False,
        log=None,
    ):
        super().__init__(
            nodes=nodes,
            cores_per_node=cores_per_node,
            gpus_per_node=gpus_per_node,
            host_prefix=host_prefix,
        )
        self.hwloc_xml_path = hwloc_xml_path
        self.verbose = verbose
        self.log = log if log is not None else _stderr_log

    # TODO: switch from `flux R encode | flux kvs put --raw` shell-out
    # to the Python KVS interface once those operations are convenient
    # via flux.kvs. The shell-out works today and matches the
    # prototype.

    def install(self, handle, scheduler="sched-simple", scheduler_options=None):
        """Encode R, install it, and swap the scheduler around a
        resource module reload.

        Args:
          handle: an open :class:`flux.Flux` handle.
          scheduler: scheduler module name to load after the
              resource module reload. Defaults to ``"sched-simple"``.
          scheduler_options: optional string of module arguments
              for the scheduler, parsed with :func:`shlex.split`.
        """
        r_json, hwloc_xml = self._encode_R()
        r_json = self._apply_amend_R(r_json, hwloc_xml)
        self._install_R(r_json)
        self._unload_current_scheduler(handle)
        self._reload_resource_module(handle)
        self._load_scheduler(handle, scheduler, scheduler_options)
        self.log("Fake resources injected.")

    def _encode_R(self):
        """Run ``flux R encode`` and return ``(r_json, hwloc_xml_or_None)``."""
        if self.hwloc_xml_path:
            cmd = ["flux", "R", "encode", "--local"]
            env = os.environ.copy()
            env["HWLOC_XMLFILE"] = self.hwloc_xml_path
            if self.verbose:
                self.log(f"+ HWLOC_XMLFILE={self.hwloc_xml_path} " + " ".join(cmd))
            result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, check=True)
            with open(self.hwloc_xml_path) as f:
                hwloc_xml = f.read()
            return result.stdout, hwloc_xml

        cmd = build_R_encode_args(self)
        self.log(
            f"Encoding fake R: {self.nodes} nodes, "
            f"{self.cores_per_node} cores/node, "
            f"{self.gpus_per_node} GPUs/node"
        )
        if self.verbose:
            self.log("+ " + " ".join(cmd))
        result = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
        return result.stdout, None

    def _apply_amend_R(self, r_json, hwloc_xml):
        """Round-trip R through the amend_R hook."""
        R = json.loads(r_json)
        R = self.amend_R(R, hwloc_xml=hwloc_xml)
        return json.dumps(R).encode()

    def _install_R(self, r_json):
        """Write R into the ``resource.R`` KVS key."""
        cmd = ["flux", "kvs", "put", "--raw", "resource.R=-"]
        if self.verbose:
            self.log("+ ... | " + " ".join(cmd))
        subprocess.run(cmd, input=r_json, check=True)

    def _unload_current_scheduler(self, handle):
        """Remove the module currently providing the ``sched`` service, if any."""
        current = ModuleList(handle).lookup("sched")
        if current is None:
            return
        if self.verbose:
            self.log(f"+ flux module remove {current}")
        handle.rpc("module.remove", {"name": current}).get()

    def _reload_resource_module(self, handle):
        """Reload the resource module with ``noverify monitor-force-up``."""
        if self.verbose:
            self.log("+ flux module reload resource noverify monitor-force-up")
        handle.rpc("module.remove", {"name": "resource"}).get()
        handle.rpc(
            "module.load",
            {
                "path": "resource",
                "args": ["noverify", "monitor-force-up"],
                "exec": False,
            },
        ).get()

    def _load_scheduler(self, handle, name, options):
        """Load the named scheduler module with the given options string."""
        args = shlex.split(options) if options else []
        if self.verbose:
            arg_str = " ".join(args)
            self.log(f"+ flux module load {name}" + (f" {arg_str}" if arg_str else ""))
        handle.rpc(
            "module.load",
            {"path": name, "args": args, "exec": False},
        ).get()


def reload_scheduler(handle, name, options=None):
    """Unload the current scheduler (if any) and load ``name`` in its place.

    Uses :class:`flux.modprobe.ModuleList` to look up which module
    currently provides the ``sched`` service; if any, removes it,
    then loads ``name`` with ``options`` parsed via :func:`shlex.split`.

    Args:
      handle: an open :class:`flux.Flux` handle.
      name: scheduler module name (e.g. ``"sched-simple"``,
          ``"sched-fluxion-qmanager"``).
      options: optional string of module arguments. Parsed with
          :func:`shlex.split` and passed as an array; passing a
          single un-split string triggers EPROTO on ``module.load``.
    """
    current = ModuleList(handle).lookup("sched")
    if current is not None:
        handle.rpc("module.remove", {"name": current}).get()
    args = shlex.split(options) if options else []
    handle.rpc(
        "module.load",
        {"path": name, "args": args, "exec": False},
    ).get()


# vi: ts=4 sw=4 expandtab
