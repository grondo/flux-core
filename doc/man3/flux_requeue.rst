===============
flux_requeue(3)
===============

.. default-domain:: c

SYNOPSIS
========

.. code-block:: c

  #include <flux/core.h>

  int flux_requeue (flux_t *h, const flux_msg_t *msg, int flags);

Link with :command:`-lflux-core`.

DESCRIPTION
===========

:func:`flux_requeue` requeues a :var:`msg` in handle :var:`h`. The message
can be received with :man3:`flux_recv` as though it arrived from the broker.

:var:`flags` must be set to one of the following values:

FLUX_RQ_TAIL
   :var:`msg` is placed at the tail of the message queue.

FLUX_RQ_TAIL
   :var:`msg` is placed at the head of the message queue.


RETURN VALUE
============

:func:`flux_requeue` return zero on success.
On error, -1 is returned, and :var:`errno` is set appropriately.


ERRORS
======

EINVAL
   Some arguments were invalid.

ENOMEM
   Out of memory.


RESOURCES
=========

.. include:: common/resources.rst


SEE ALSO
========

:man3:`flux_open`, :man3:`flux_recv`, :man3:`flux_send`
