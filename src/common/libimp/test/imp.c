#if HAVE_CONFIG_H
#include "config.h"
#endif
#include "src/common/libtap/tap.h"
#include "src/common/libimp/imp.h"

int main(int argc, char** argv)
{
    flux_imp_t *imp;

    plan (NO_PLAN);

    imp = flux_imp_create ();
    ok (imp != NULL,
        "flux_imp_create works");

    flux_imp_destroy (imp);

    done_testing();
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
