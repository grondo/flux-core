###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Required properties of an RFC 31 constraint."""

import json
from collections.abc import Mapping

__all__ = ["requires"]


def requires(constraint):
    """Return the set of properties *constraint* guarantees a rank must have.

    This is the opt-in set of a job: a rank that carries an exclusive
    property may be allocated to the job only if that property is in this
    set.  It is computed from the structure of the constraint::

        properties: [...]   the names, without a '^' prefix
        and: [...]          union of the children
        or: [...]           intersection of the children
        not: [...]          the empty set
        no constraint       the empty set

    An unknown operator contributes nothing, so an unrecognized constraint
    can never widen the set.

    Args:
        constraint: An RFC 31 constraint as a dict, a JSON string, or None.

    Returns:
        set: The required property names.
    """
    if constraint is None:
        return set()
    if isinstance(constraint, str):
        constraint = json.loads(constraint)
    if not isinstance(constraint, Mapping):
        raise ValueError("constraint must be an object")

    result = set()
    for operator, operand in constraint.items():
        if operator == "properties":
            result |= {p for p in operand if not p.startswith("^")}
        elif operator == "and":
            for child in operand:
                result |= requires(child)
        elif operator == "or":
            # Only a property required by every branch is guaranteed.  An
            # empty 'or' matches nothing, so it guarantees nothing.
            terms = [requires(child) for child in operand]
            if terms:
                result |= set.intersection(*terms)
        # 'not' and unknown operators guarantee nothing.
    return result
