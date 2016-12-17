#ifndef _FLUX_CORE_MODSERVICE_H
#define _FLUX_CORE_MODSERVICE_H


/*
 *  Flux extension/module loading service:
 */
typedef struct flux_module_service flux_extensor_t;

/*
 *  Generic flux module type:
 */
typedef struct flux_module_handle flux_module_t;

/*
 *  Module loader implementation details.
 *  Loaders are registered with the extensor, and should provide a
 *   "name" and list of extensions (NULL terminated) that they support.
 */
struct flux_module_loader {
    char *name;
    int (*init) (flux_module_t *p, const char *path, int flags);
    int (*load) (flux_module_t *p);
    int (*unload) (flux_module_t *p);
    void (*destroy) (flux_module_t *p);
    void * (*lookup) (flux_module_t *p, const char *symbol);
    const char * (*get_name) (flux_module_t *p);
    const char * (*strerror) (flux_module_t *p);

    /* NULL terminated list of supported file extensions */
    char *extensions[];
};

/*
 *  Create/destroy extensor object for loading generic Flux extensions:
 */
flux_extensor_t * flux_extensor_create ();
void flux_extensor_destroy (flux_extensor_t *);

/*
 *  Register a module loader implementation with extensor `m`
 */
int flux_extensor_register_loader (flux_extensor_t *e,
    struct flux_module_loader *l);

/*
 *  Return loader implementation for loader name, if registered in current
 *   extensor instance.
 */
struct flux_module_loader * flux_extensor_get_loader (flux_extensor_t *e,
    const char *name);

/*
 *  Load all possible modules under search path `path` into the
 *   extensor `m`. The total number of modules loaded will be returned,
 *   or -1 on error.
 */
int flux_extensor_loadall (flux_extensor_t *e, const char * path);

/*
 *  Load first module in searchpath matching `name`:
 */
flux_module_t * flux_extensor_find_module (flux_extensor_t *e,
    const char *searchpath, const char *name);

/*
 *  Get the module currently registered under `name` from extensor `m`.
 *
 *  Note that if multiple loaded modules provide `name`, the first loaded
 *  under that name wins. Once the current module registered under `name`
 *  is unloaded, then the next module with name, if one exists, will replace
 *  it on the next call to `flux_extensor_get_module`.
 */
flux_module_t * flux_extensor_get_module (flux_extensor_t *e, const char *name);

/*
 *  Create module at path with extensor `m`, returning the module handle on
 *   success, or NULL otherwise. The default loader for `path` based on
 *   extension will be used to load the module.
 *
 *  Note this function does not "load" the module, and the module's name
 *   is not automatically bound in extensor `m`. Typically, flux_module_load
 *   will be called immediately after this function, however, access
 *   to the module handle before and after "load" eases error handling
 *   since flux_module_strerror() can be used to query the last error
 *   encountered during load().
 */
flux_module_t * flux_module_create (flux_extensor_t *e,
    const char *path, int flags);

/*
 *  Like flux_module_create(), but force loader to `name`.
 */
flux_module_t * flux_module_create_with_loader (flux_extensor_t *e,
    const char *loader, const char *path, int flags);

/*
 *  Flux module handle (flux_module_t) interface functions:
 */

/*  Get and set arbitrary context under module `p`, meant to be used by
 *   module loader implementations.
 */
void * flux_module_getctx (flux_module_t *p);
void * flux_module_setctx (flux_module_t *p, void *data);


/*  Load/unload/destroy modules via their handles */
int flux_module_load (flux_module_t *p);
int flux_module_unload (flux_module_t *p);
void flux_module_destroy (flux_module_t *p);

/*
 *  Accessors provided by all modules:
 */
const char * flux_module_path (flux_module_t *p);
const char * flux_module_name (flux_module_t *p);
const char * flux_module_strerror (flux_module_t *p);
const char * flux_module_uuid (flux_module_t *p);

/*
 *  Generic "lookup" function. Whether this function works is loader
 *   dependent. On failure, ENOSYS or other appropriate error will be
 *   returned.
 */
void *flux_module_lookup (flux_module_t *p, const char *symbol);

#endif /* !FLUX_CORE_MODSERVICE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */