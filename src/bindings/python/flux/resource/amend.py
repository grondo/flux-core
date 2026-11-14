###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Load R amenders.

An amender is a callable ``amend(R, hwloc_xml=None) -> R`` that adds
scheduler-specific data, such as the ``scheduling`` key, to an R object.
Amenders are named by the ``amend-r`` key in the ``[resource]`` and
``[fake-resources]`` config tables.
"""

import importlib
import os

from flux.importer import import_path


def load_amender(spec):
    """Resolve an ``amend-r`` spec to a callable.

    Three forms are supported:

    * ``module.path:function_name`` — imports ``module.path`` and returns
      its ``function_name`` attribute.

    * A path to a Python file, i.e. a spec that contains ``/`` or ends in
      ``.py`` — the file is loaded as a module and its ``amend`` callable
      is returned.

    * A plain name — ``flux.resource.<name>:amend`` is used, so that
      e.g. ``TreePool`` selects :func:`flux.resource.TreePool.amend`.

    Raises :class:`RuntimeError` with a descriptive message on any failure
    (file not found, import error, missing attribute), so configuration
    mistakes surface clearly from the modprobe rc1 task rather than as a
    generic ImportError.
    """
    if ":" in spec:
        mod_name, fn_name = spec.split(":", 1)
        return _load_from_module(spec, mod_name, fn_name)
    if os.sep not in spec and not spec.endswith(".py"):
        return _load_from_module(spec, f"flux.resource.{spec}", "amend")

    path = os.path.expanduser(spec)
    if not os.path.isfile(path):
        raise RuntimeError(f"amend-r: file not found: {path!r}")
    try:
        module = import_path(path)
    except Exception as exc:
        raise RuntimeError(f"amend-r: failed to load {path!r}: {exc}")
    if not hasattr(module, "amend"):
        raise RuntimeError(
            f"amend-r: file {path!r} has no 'amend' callable at module scope"
        )
    return module.amend


def _load_from_module(spec, mod_name, fn_name):
    try:
        module = importlib.import_module(mod_name)
    except ImportError as exc:
        raise RuntimeError(
            f"amend-r: {spec!r}: failed to import module {mod_name!r}: {exc}"
        )
    if not hasattr(module, fn_name):
        raise RuntimeError(
            f"amend-r: {spec!r}: module {mod_name!r} has no attribute {fn_name!r}"
        )
    return getattr(module, fn_name)
