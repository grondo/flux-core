====================
flux-config-sched(5)
====================


DESCRIPTION
===========

The ``sched`` table may be used to select the scheduler of a Flux instance
and to prepare R for it at startup. Since the table is read when the
instance starts, it is most useful in a configuration passed to
:man1:`flux-batch` or :man1:`flux-alloc` with :option:`flux-batch --conf`,
where it selects a scheduler for a single job without any change to the
system configuration.

The ``sched`` table may contain the following keys:


KEYS
====

module
   (optional) The name of the scheduler module to load, for example
   ``"sched-simple"`` or ``"sched-fifo"``. This is a shorthand for
   ``modules.alternatives.sched`` (see :man1:`flux-modprobe`), and takes
   precedence over it.

amend-r
   (optional) Name a Python amender that adds a RFC 20 ``scheduling`` key
   to R at instance startup, before the scheduler is loaded. Any existing
   ``scheduling`` key, such as one inherited from the enclosing instance,
   is replaced. The amender is passed R and the hwloc XML of rank 0, so
   all nodes are assumed to have the same topology. The value may be:

   * A plain name, which selects the ``amend`` function of the
     ``flux.resource.<name>`` module. For example, ``"TreePool"`` adds the
     topology required by the ``TreePool`` resource pool, which
     sched-simple then selects automatically.
   * ``module.path:function_name``, which names a function in a Python
     module on PYTHONPATH.
   * A path to a Python file containing an ``amend`` function. The path
     must contain ``/`` or end in ``.py``.

   The amender signature is ``amend(R, hwloc_xml=None) -> R``. See
   :man5:`flux-config-fake-resources` for more on writing amenders.


EXAMPLES
========

A configuration that selects sched-simple with locality-aware ``TreePool``
scheduling of GPUs and cores:

::

   [sched]
   module = "sched-simple"
   amend-r = "TreePool"

If this configuration is saved as ``treepool.toml`` in a directory of the
named configuration search path, e.g. ``~/.config/flux/config``, it may be
used by name:

.. code-block:: console

   $ flux batch -N2 --exclusive --conf=treepool script.sh

The same keys may be set directly on the command line:

.. code-block:: console

   $ flux alloc -N2 --exclusive \
       --conf=sched.module=sched-simple --conf=sched.amend-r=TreePool


RESOURCES
=========

.. include:: common/resources.rst


FLUX RFC
========

:doc:`rfc:spec_20`


SEE ALSO
========

:man5:`flux-config`, :man5:`flux-config-fake-resources`,
:man1:`flux-batch`, :man1:`flux-alloc`, :man1:`flux-modprobe`
