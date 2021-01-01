###############################################################
# Copyright 2014 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

import abc
import errno
import signal
from flux.core.inner import raw, lib, ffi

__all__ = ["TimerWatcher", "FDWatcher", "SignalWatcher"]


class Watcher(object):
    __metaclass__ = abc.ABCMeta

    def __init__(self, handle=None):
        self.handle = handle

    def __enter__(self):
        """Allow this to be used as a context manager"""
        self.start()
        return self

    def __exit__(self, type_arg, value, _):
        """Allow this to be used as a context manager"""
        self.stop()
        return False

    def __del__(self):
        if self.handle is not None:
            self.destroy()

    def start(self):
        raw.flux_watcher_start(self.handle)

    def stop(self):
        raw.flux_watcher_stop(self.handle)

    def destroy(self):
        if self.handle is not None:
            raw.flux_watcher_destroy(self.handle)
            self.handle = None


def watcher_compat_mode(watcher, compat_count):
    """
    Simulate old-style callback when callback has ``compat_count`` args AND
     watcher.kwargs is either empty (args == None) or has one entry
     watcher.kwargs["args"]. Then, ``args`` will be passed as the last
     parameter to watcher.callback().
    """

    if watcher.callback.__code__.co_argcount != compat_count:
        #  If argument count doesn't match compat_count, then do not enable
        #   compat mode:
        #
        return False

    if len(watcher.kwargs) == 0:
        #  If no extra keyword args were passed, but argument count
        #   expects extra args, then pass either the first positional
        #   argument, or None.
        #
        if len(watcher.args) == 1:
            watcher.kwargs["args"] = watcher.args[0]
            watcher.args = ()
        else:
            watcher.kwargs["args"] = None
    elif "args" not in watcher.kwargs:
        #  O/w, if 'args' was not explicitly set as a keywork argument,
        #   then this must not be an old style callback
        #
        return False

    return True


@ffi.def_extern()
def timeout_handler_wrapper(unused1, unused2, revents, opaque_handle):
    del unused1, unused2  # unused arguments
    watcher = ffi.from_handle(opaque_handle)
    try:
        if not watcher.compat_mode:
            watcher.callback(
                watcher.flux_handle, watcher, revents, *watcher.args, **watcher.kwargs
            )
        else:
            watcher.callback(
                watcher.flux_handle, watcher, revents, watcher.kwargs["args"]
            )
    # pylint: disable=broad-except
    except Exception as exc:
        type(watcher.flux_handle).set_exception(exc)
        watcher.flux_handle.reactor_stop_error()


class TimerWatcher(Watcher):
    def __init__(self, flux_handle, after, callback, *args, repeat=0, **kwargs):
        self.flux_handle = flux_handle
        self.after = after
        self.repeat = repeat
        self.callback = callback
        self.args = args
        self.kwargs = kwargs
        self.handle = None
        self.wargs = ffi.new_handle(self)
        # cb args compatibility mode:
        self.compat_mode = watcher_compat_mode(self, 4)
        super(TimerWatcher, self).__init__(
            raw.flux_timer_watcher_create(
                raw.flux_get_reactor(flux_handle),
                float(after),
                float(repeat),
                lib.timeout_handler_wrapper,
                self.wargs,
            )
        )


@ffi.def_extern()
def fd_handler_wrapper(unused1, unused2, revents, opaque_handle):
    del unused1, unused2  # unused arguments
    watcher = ffi.from_handle(opaque_handle)
    try:
        fd_int = raw.fd_watcher_get_fd(watcher.handle)
        if not watcher.compat_mode:
            watcher.callback(
                watcher.flux_handle,
                watcher,
                fd_int,
                revents,
                *watcher.args,
                **watcher.kwargs
            )
        else:
            watcher.callback(
                watcher.flux_handle, watcher, fd_int, revents, watcher.kwargs["args"]
            )
    # pylint: disable=broad-except
    except Exception as exc:
        type(watcher.flux_handle).set_exception(exc)
        watcher.flux_handle.reactor_stop_error()


class FDWatcher(Watcher):
    def __init__(self, flux_handle, fd_int, events, callback, *args, **kwargs):
        self.flux_handle = flux_handle
        self.fd_int = fd_int
        self.events = events
        self.callback = callback
        self.args = args
        self.kwargs = kwargs
        self.handle = None
        self.wargs = ffi.new_handle(self)
        self.compat_mode = watcher_compat_mode(self, 5)
        super(FDWatcher, self).__init__(
            raw.flux_fd_watcher_create(
                raw.flux_get_reactor(flux_handle),
                self.fd_int,
                self.events,
                lib.fd_handler_wrapper,
                self.wargs,
            )
        )


@ffi.def_extern()
def signal_handler_wrapper(_unused1, _unused2, _unused3, opaque_handle):
    watcher = ffi.from_handle(opaque_handle)
    try:
        signal_int = raw.signal_watcher_get_signum(watcher.handle)
        if not watcher.compat_mode:
            watcher.callback(
                watcher.flux_handle,
                watcher,
                signal_int,
                *watcher.args,
                **watcher.kwargs
            )
        else:
            watcher.callback(
                watcher.flux_handle, watcher, signal_int, watcher.kwargs["args"]
            )
    # pylint: disable=broad-except
    except Exception as exc:
        type(watcher.flux_handle).set_exception(exc)
        watcher.flux_handle.reactor_stop_error()


class SignalWatcher(Watcher):
    def __init__(self, flux_handle, signal_int, callback, *args, **kwargs):
        self.flux_handle = flux_handle
        self.signal_int = signal_int
        self.callback = callback
        self.args = args
        self.kwargs = kwargs
        self.handle = None
        self.wargs = ffi.new_handle(self)
        self.compat_mode = watcher_compat_mode(self, 4)
        super(SignalWatcher, self).__init__(
            raw.flux_signal_watcher_create(
                raw.flux_get_reactor(flux_handle),
                self.signal_int,
                lib.signal_handler_wrapper,
                self.wargs,
            )
        )
        # N.B.: check for error only after SignalWatcher object fully
        #  initialized to avoid 'no attribute self.handle' in __del__
        #  method.
        if signal_int < 1 or signal_int >= signal.NSIG:
            raise OSError(errno.EINVAL, "invalid signal number")


# vi: ts=4 sw=4 expandtab
