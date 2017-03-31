#ifndef _FLUX_LIBIMP_IMP_H
#define _FLUX_LIBIMP_IMP_H

typedef struct flux_imp flux_imp_t;

flux_imp_t *flux_imp_create (void);
void flux_imp_destroy (flux_imp_t *imp);

#endif /* !_FLUX_LIBIMP_IMP_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
