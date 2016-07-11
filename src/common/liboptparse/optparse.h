#ifndef _UTIL_OPTPARSE_H
#define _UTIL_OPTPARSE_H

#include <stdbool.h>

/******************************************************************************
 *  Datatypes:
 *****************************************************************************/
struct optparse_option;
typedef struct opt_parser optparse_t;

/*
 *  prototype for output function used by optparser
 */
typedef int (*opt_log_f) (const char *fmt, ...);

/*
 *  prototype for fatal error function
 */
typedef int (*opt_fatalerr_f) (void *h, int exit_code);

/*
 *  prototype for option callback hook
 */
typedef int (*optparse_cb_f) (optparse_t *p, struct optparse_option *,
			      const char *optarg);

/*
 *  prototype for subcommand callback
 */
typedef int (*optparse_subcmd_f) (optparse_t *p, int ac, char **av);

/*
 *  Error codes:
 */
typedef enum {
   OPTPARSE_SUCCESS,       /* Success                                      */
   OPTPARSE_FAILURE,       /* Generic error.                               */
   OPTPARSE_NOMEM,         /* Memory allocation failure                    */
   OPTPARSE_BAD_ARG,       /* Invalid argument                             */
   OPTPARSE_EEXIST,        /* Option exists                                */
   OPTPARSE_NOT_IMPL,      /* Not implemented                              */
} optparse_err_t;

/*
 *  Set of item types for optparse_get and optparse_set:
 */
typedef enum {
    OPTPARSE_USAGE,        /* Set usage message in --help output (char *)   */
    OPTPARSE_LOG_FN,       /* Set log function (default fprintf(stderr,..)) */
    OPTPARSE_FATALERR_FN,  /* Set fatal err function (default: exit() )     */
    OPTPARSE_FATALERR_HANDLE,  /* Set handle passed to fatalerr function    */
    OPTPARSE_OPTION_WIDTH, /* Width allotted to options in --help output    */
    OPTPARSE_LEFT_MARGIN,  /* Left pad for option output (default = 2)      */
    OPTPARSE_PRINT_SUBCMDS,/* Print all subcommands in --help (default = T  */
    OPTPARSE_SUBCMD_NOOPTS,/* Don't parse options for this subcommand       */
} optparse_item_t;

/*
 *  Description of an option:
 */
#define list_argument 3
struct optparse_option {
    const char *  name;    /*  Option name (e.g. "help" for --help)         */
    int           key;     /*  Option key  (e.g. 'h', or other number).
				If !isalnum(key), then this option is
                                assumed to be a long option only.           */

    int           has_arg; /*  0: no arg, 1: req'd arg, 2: optional arg
                               3: list-arg (split on comma separate values) */
    int           group;   /*  Grouping in --help output                    */
    const char *  arginfo; /*  arg info displayed after = in help output    */
    const char *  usage;   /*  String for usage/help output                 */
    optparse_cb_f cb;      /*  Callback function                            */
};

#define OPTPARSE_TABLE_END { NULL, 0, 0, 0, NULL, NULL, NULL }

/*
 *  Description of a subcommand:
 */
struct optparse_subcommand {
    const char *            name;   /* Subcommand name                      */
    const char *            usage;  /* Usage string                         */
    const char *            doc;    /* Short documentation string           */
    optparse_subcmd_f       fn;     /* Subcommand function                  */
    struct optparse_option *opts;   /* Table of optparse_options            */
};

#define OPTPARSE_SUBCMD_END { NULL, NULL, NULL, NULL, NULL }

/******************************************************************************
 *  Prototypes:
 *****************************************************************************/

/*
 *   Create an optparse object for program named [program_name]
 */
optparse_t *optparse_create (const char *program_name);


/*
 *   Create a subcommand optparse object as a child of [p].
 *    [name] is subcommand name for subcmd callback [cb].
 *
 *   Returns an optparse object for the subcommand, which can be used
 *    to register subcommand options, set usage, etc.
 */
optparse_t *optparse_add_subcommand (optparse_t *p,
                                     const char *name,
                                     optparse_subcmd_f cb);

/*
 *   Get subcommand optparse object from parent [p], or NULL if subcommand
 *    [name] does not exist.
 */
optparse_t *optparse_get_subcommand (optparse_t *p, const char *name);

/*   Get parent optparse object for a subcommand, or NULL if [p] is not
 *    a subcommand optparse object.
 */
optparse_t *optparse_get_parent (optparse_t *p);

/*
 *   Convenience function like optparse_add_subcommand, additionally
 *    registering with usage string [usage], documentation blurb [doc] and
 *    any options from optparse_option table [opts]. Use
 *    optparse_get_subcommand() to get subcommand optparse handle.
 *
 *   Returns OPTPARSE_SUCCESS on successful registration, or an optparse_err_t
 *    on failure.
 */
optparse_err_t optparse_reg_subcommand (optparse_t *p,
                                     const char *name,
                                     optparse_subcmd_f cb,
                                     const char *usage,
                                     const char *doc,
                                     struct optparse_option const opts[]);


/*
 *   Register a table of struct optparse_subcommand subcommands in a
 *    single call.
 *
 *   Returns OPTPARSE_SUCCESS if all subcommands registered successfully,
 *    or optparse_err_t on failure.
 */
optparse_err_t optparse_reg_subcommands (optparse_t *p,
                                     struct optparse_subcommand cmds[]);

/*
 *   Destroy program options handle [p].
 */
void optparse_destroy (optparse_t *p);

/*
 *   Register the option [o] with the program options object [p].
 *
 *   Returns OPTPARSE_SUCCESS if the option was successfully registered
 *    with the parser [p]. Otherwise, one of the following error codes
 *    is returned:
 *
 *    OPTPARSE_NOMEM:  Failed to allocate memory for some options.
 *    OPTPARSE_EEXIST: An attempt to register a duplicate option was detected.
 *    OPTPARSE_EINVAL: The optparse_t *object is currupt or invalid.
 */
optparse_err_t optparse_add_option (optparse_t *p,
                                    const struct optparse_option *o);

/*
 *   Remove option [name] from parser [p].
 */
optparse_err_t optparse_remove_option (optparse_t *p, const char *name);

/*
 *   Register all program options in table [opts] to the program options
 *    object [p]. [opts] should be an array of struct optparse_option
 *    and the last entry should be PROG_OPTS_TABLE_END.
 *
 *   Returns OPTPARSE_SUCCESS if all options are successfully registered
 *    with the parser [p]. Otherwise returns an error as in
 *    optparse_add_option().
 */
optparse_err_t optparse_add_option_table (optparse_t *p,
	                                  struct optparse_option const opts[]);

/*
 *   Register a doc string [doc] for display in autogenerated --help output
 *    for program options object [p]. The doc string will preceed the
 *    option output for group [group].
 */
optparse_err_t optparse_add_doc (optparse_t *p, const char *doc, int group);

optparse_err_t optparse_set (optparse_t *p, optparse_item_t item, ...);

optparse_err_t optparse_get (optparse_t *p, optparse_item_t item, ...);

/*   Set and get arbitrary ancillary data associated with an optparse
 *    object. optparse_get_data () returns NULL if data not found.
 *
 *   If [p] is a subcommand and [name] does not exist for the current
 *    optparse object, optparse_get_data will recursively search parent
 *    for the named data item.
 */
void optparse_set_data (optparse_t *p, const char *name, void *data);

void * optparse_get_data (optparse_t *p, const char *name);

/*
 *   Print the usage output for program options object [p] using the
 *    registered output function.
 */
int optparse_print_usage (optparse_t *p);

/*
 *   Print a message using [fmt, ...] using registered log function,
 *    followed by a the help message for optparse object [p],
 *    then call registered fatalerr function with [code].
 *
 *   (By default this function will print the error to stderr, followed
 *    by the help for [p], then exit with exit status [code])
 *
 *   Error message, if provided, will always be prefixed with the full
 *    "program name" for optparse object [p].
 *
 *   Returns the return value of registered fatalerr function, if the
 *    function returns at all.
 */
int optparse_fatal_usage (optparse_t *p, int code, const char *fmt, ...);

/*
 *   Process command line args in [argc] and [argv] using the options
 *    defined in program options object [p]. This will result in callbacks
 *    being called as options are parsed, or option and argument usage
 *    can be checked after successful completion with
 *    optparse_options_getopt() as defined below.
 *
 *   Returns -1 on failure, first non-option index in argv on success.
 */
int optparse_parse_args (optparse_t *p, int argc, char *argv[]);

/*
 *   Run any defined subcommand callback of the optparse object [p]
 *    using the first non-option argument in [argc,argv]. The callback
 *    function is passed a reference to its own optparse object, with
 *    sub-options already processed with optparse_parse_args(), and
 *    [argc,argv] adjusted for the subcommand (i.e. argv[0] will equal
 *    the subcommand name).
 *
 *   If OPTPARSE_SUBCMD_NOOPTS is set, auto optparse_parse_args()
 *    for subcommand options is skipped.
 *
 *   This function can be called either before or after a call to
 *    optparse_parse_args (p, ...), optparse_run_subcommand() will call
 *    first-level option processing if [p] has not been initialized by
 *    a call to optparse_parse_args.
 *
 *   Returns the value returned by subcommand callback or prints
 *    error and returns fatalerr() on error.
 */
int optparse_run_subcommand (optparse_t *p, int argc, char *argv[]);

/*
 *   After a call to optparse_parse_args (), return the number of times the
 *     option 'name' was used, or 0 if not. If the option was used and it takes
 *    an argument, then that argument is passed back in [optargp].
 */
int optparse_getopt (optparse_t *p, const char *name, const char **optargp);


/*
 *   Iterate over multiple optarg values for options that were provided
 *    more than once. Returns NULL at end of list, or if option "name"
 *    was not found (in which case optparse_getopt_iterator_reset()
 *    for "name" will return -1).
 */
const char *optparse_getopt_next (optparse_t *p, const char *name);

/*
 *   Reset internal iterator so that optparse_getopt_next() will return the
 *    first argument from the list. Returns the number of items to iterate, or
 *    -1 if option "name" not found.
 */
int optparse_getopt_iterator_reset (optparse_t *p, const char *name);

/*
 *   Return true if the option 'name' was used, false if not.
 *    If the option is unknown, log an error and call exit (1).
 */
bool optparse_hasopt (optparse_t *p, const char *name);

/*
 *   Return the option argument as an integer if 'name' was used,
 *    'default_value' if not.  If the option is unknown, or the argument
 *    could not be converted to an integer, call the fatal error function.
 */
int optparse_get_int (optparse_t *p, const char *name, int default_value);

/*
 *   Return the option argument as a string if 'name' was used, 'default_value'
 *    if not.  If the option is unknown, call the fatal error function.
 */
const char *optparse_get_str (optparse_t *p, const char *name,
                              const char *default_value);

/*
 *   Return optind from previous call to optparse_parse_args ().
 *    Returns -1 if  args have not yet been parsed, and thus optind is
 *    not valid.
 */
int optparse_optind (optparse_t *p);

#endif /* _UTIL_OPTPARSE_H */