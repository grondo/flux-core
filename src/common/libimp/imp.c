/*****************************************************************************\
 *  Copyright (c) 2017 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation; either version 2 of the license,
 *  or (at your option) any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include "imp.h"

#define FLUX_IMP_MAGIC 0x0e0ef0f0
struct flux_imp {
    int magic;
};

flux_imp_t *flux_imp_create (void)
{
    flux_imp_t *imp = calloc (1, sizeof (*imp));
    if (!imp) {
        errno = ENOMEM;
        goto error;
    }
    imp->magic = FLUX_IMP_MAGIC;
    return imp;
error:
    flux_imp_destroy (imp);
    return NULL;
}

void flux_imp_destroy (flux_imp_t *imp)
{
    int saved_errno = errno;

    if (imp) {
        assert (imp->magic == FLUX_IMP_MAGIC);
        imp->magic = ~FLUX_IMP_MAGIC;
        free (imp);
    }
    errno = saved_errno;
}
