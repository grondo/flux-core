#!/bin/bash
#
#  Run Flux tests as Flux jobs using bulksubmit.
#  This must be run from the sharness test directory ./t.
#
#  Usage: test-inception.sh [TEST...]
#
#  Run the given tests, or all tests if none are given. Tests are
#  submitted in the order given, so list long running tests first.
#

stats() {
    while :; do
       echo === $(flux jobs --stats-only) ===
       sleep 5
    done
}

if test $# -eq 0; then
    set -- t[0-9]*.t python/t*.py lua/t*.t
fi
sharness=()
python=()
lua=()
for t in "$@"; do
    case "$t" in
        python/*.py) python+=("$t") ;;
        lua/*.t)     lua+=("$t") ;;
        *.t)         sharness+=("$t") ;;
        *)           echo "test-inception.sh: unsupported test $t" >&2
                     exit 1 ;;
    esac
done

stats &
PID=$!
export FLUX_TESTS_LOGFILE=t
# run sharness tests with -d -v
if test ${#sharness[@]} -gt 0; then
    flux bulksubmit -o pty --quiet --job-name={./%} \
        sh -c './{} -d -v --root=$FLUX_JOB_TMPDIR' \
        ::: "${sharness[@]}"
fi
# python and lua tests do not support -d, -v, or --root options
if test ${#python[@]} -gt 0; then
    flux bulksubmit -o pty --quiet --job-name={./%} flux python {} \
        ::: "${python[@]}"
fi
if test ${#lua[@]} -gt 0; then
    flux bulksubmit -o pty --quiet --job-name={./%} ./{} ::: "${lua[@]}"
fi
flux watch --all
RC=$?
kill $PID
wait

# dump output of failed jobs to *.log
for id in $(flux jobs -f failed -no {id}); do
    name=$(flux jobs -no {name} ${id})
    flux job attach $id > ${name}.output 2>&1
done

# print failed jobs listing to stdout
flux jobs -f failed \
    -no "{id.f58:>12} {status_abbrev:>2.2} {contextual_time!F:>8h} {name}"

# exit with failure if any tests failed
exit $RC
