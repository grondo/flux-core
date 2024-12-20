###############################################################
# Copyright 2022 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

author = 'This page is maintained by the Flux community.'

# Add man page entries with the following information:
# - Relative file path (without .rst extension)
# - Man page name
# - Man page description
# - Author (use [author])
# - Manual section
#
# Note: the relative order of commands in this list affects the order
# in which commands appear within a section in the output of flux help.
# Therefore, keep these commands in relative order of importance.
#
man_pages = [
    ('man1/flux-broker', 'flux-broker', 'Flux message broker daemon', [author], 1),
    ('man1/flux-start', 'flux-start', 'bootstrap a local Flux instance', [author], 1),
    ('man1/flux-version', 'flux-version', 'Display flux version information', [author], 1),
    ('man1/flux-config', 'flux-config', 'Manage/query Flux configuration', [author], 1),
    ('man1/flux-content', 'flux-content', 'access content service', [author], 1),
    ('man1/flux-cron', 'flux-cron', 'Cron-like utility for Flux', [author], 1),
    ('man1/flux-dmesg', 'flux-dmesg', 'access broker ring buffer', [author], 1),
    ('man1/flux-dump', 'flux-dump', 'Write KVS snapshot to portable archive', [author], 1),
    ('man1/flux-env', 'flux-env', 'Print the flux environment or execute a command inside it', [author], 1),
    ('man1/flux-event', 'flux-event', 'Send and receive Flux events', [author], 1),
    ('man1/flux-exec', 'flux-exec', 'Execute processes across flux ranks', [author], 1),
    ('man1/flux-archive', 'flux-archive', 'KVS file archive utility', [author], 1),
    ('man1/flux-getattr', 'flux-setattr', 'access broker attributes', [author], 1),
    ('man1/flux-getattr', 'flux-lsattr', 'access broker attributes', [author], 1),
    ('man1/flux-getattr', 'flux-getattr', 'access broker attributes', [author], 1),
    ('man1/flux-jobs', 'flux-jobs', 'list jobs submitted to Flux', [author], 1),
    ('man1/flux-top', 'flux-top', 'Display running Flux jobs', [author], 1),
    ('man1/flux-pstree', 'flux-pstree', 'display job hierarchies', [author], 1),
    ('man1/flux-cancel', 'flux-cancel', 'cancel one or more jobs', [author], 1),
    ('man1/flux-pgrep', 'flux-pgrep', 'search or cancel matching jobs', [author], 1),
    ('man1/flux-pgrep', 'flux-pkill', 'search or cancel matching jobs', [author], 1),
    ('man1/flux-pmi', 'flux-pmi', 'PMI client test tool', [author], 1),
    ('man1/flux-jobtap', 'flux-jobtap', 'List, remove, and load job-manager plugins', [author], 1),
    ('man1/flux-shutdown', 'flux-shutdown', 'Shut down a Flux instance', [author], 1),
    ('man1/flux-uri', 'flux-uri', 'resolve Flux URIs', [author], 1),
    ('man1/flux-resource', 'flux-resource', 'list/manipulate Flux resource status', [author], 1),
    ('man1/flux-queue', 'flux-queue', 'manage the job queue', [author], 1),
    ('man1/flux-restore', 'flux-restore', 'Read KVS snapshot from portable archive', [author], 1),
    ('man1/flux-keygen', 'flux-keygen', 'generate keys for Flux security', [author], 1),
    ('man1/flux-kvs', 'flux-kvs', 'Flux key-value store utility', [author], 1),
    ('man1/flux-logger', 'flux-logger', 'create a Flux log entry', [author], 1),
    ('man1/flux-submit', 'flux-submit', 'submit a job to a Flux instance', [author], 1),
    ('man1/flux-run', 'flux-run', 'run a Flux job interactively', [author], 1),
    ('man1/flux-bulksubmit', 'flux-bulksubmit', 'submit jobs in bulk to a Flux instance', [author], 1),
    ('man1/flux-alloc', 'flux-alloc', 'allocate a new Flux instance for interactive use', [author], 1),
    ('man1/flux-batch', 'flux-batch', 'submit a batch script to Flux', [author], 1),
    ('man1/flux-job', 'flux-job', 'Job Housekeeping Tool', [author], 1),
    ('man1/flux-module', 'flux-module', 'manage Flux extension modules', [author], 1),
    ('man1/flux-overlay', 'flux-overlay', 'Show flux overlay network status', [author], 1),
    ('man1/flux-uptime', 'flux-uptime', 'Tell how long Flux has been up and running', [author], 1),
    ('man1/flux-ping', 'flux-ping', 'measure round-trip latency to Flux services', [author], 1),
    ('man1/flux-proxy', 'flux-proxy', 'create proxy environment for Flux instance', [author], 1),
    ('man1/flux-startlog', 'flux-startlog', 'Show Flux instance start and stop times', [author], 1),
    ('man1/flux', 'flux', 'the Flux resource management framework', [author], 1),
    ('man1/flux-shell', 'flux-shell', 'the Flux job shell', [author], 1),
    ('man1/flux-watch', 'flux-watch', 'monitor one or more Flux jobs', [author], 1),
    ('man1/flux-update', 'flux-update', 'update active Flux jobs', [author], 1),
    ('man1/flux-hostlist', 'flux-hostlist', 'fetch, combine, and manipulate Flux hostlists', [author], 1),
    ('man1/flux-housekeeping', 'flux-housekeeping', 'list and terminate housekeeping tasks', [author], 1),
    ('man3/flux_attr_get', 'flux_attr_set', 'get/set Flux broker attributes', [author], 3),
    ('man3/flux_attr_get', 'flux_attr_get', 'get/set Flux broker attributes', [author], 3),
    ('man3/flux_aux_set', 'flux_aux_get', 'get/set auxiliary handle data', [author], 3),
    ('man3/flux_aux_set', 'flux_aux_set', 'get/set auxiliary handle data', [author], 3),
    ('man3/flux_child_watcher_create', 'flux_child_watcher_get_rpid', 'create child watcher', [author], 3),
    ('man3/flux_child_watcher_create', 'flux_child_watcher_get_rstatus', 'create child watcher', [author], 3),
    ('man3/flux_child_watcher_create', 'flux_child_watcher_create', 'create child watcher', [author], 3),
    ('man3/flux_core_version', 'flux_core_version_string', 'get flux-core version', [author], 3),
    ('man3/flux_core_version', 'flux_core_version', 'get flux-core version', [author], 3),
    ('man3/flux_event_decode', 'flux_event_decode_raw', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_decode', 'flux_event_unpack', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_decode', 'flux_event_encode', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_decode', 'flux_event_encode_raw', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_decode', 'flux_event_pack', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_decode', 'flux_event_decode', 'encode/decode a Flux event message', [author], 3),
    ('man3/flux_event_publish', 'flux_event_publish_pack', 'publish events', [author], 3),
    ('man3/flux_event_publish', 'flux_event_publish_raw', 'publish events', [author], 3),
    ('man3/flux_event_publish', 'flux_event_publish_get_seq', 'publish events', [author], 3),
    ('man3/flux_event_publish', 'flux_event_publish', 'publish events', [author], 3),
    ('man3/flux_event_subscribe', 'flux_event_unsubscribe', 'manage subscriptions', [author], 3),
    ('man3/flux_event_subscribe', 'flux_event_subscribe', 'manage subscriptions', [author], 3),
    ('man3/flux_comms_error_set', 'flux_comms_error_set', 'register callback for communications errors', [author], 3),
    ('man3/flux_fd_watcher_create', 'flux_fd_watcher_get_fd', 'create file descriptor watcher', [author], 3),
    ('man3/flux_fd_watcher_create', 'flux_fd_watcher_create', 'create file descriptor watcher', [author], 3),
    ('man3/flux_flags_set', 'flux_flags_unset', 'manipulate Flux handle flags', [author], 3),
    ('man3/flux_flags_set', 'flux_flags_get', 'manipulate Flux handle flags', [author], 3),
    ('man3/flux_flags_set', 'flux_flags_set', 'manipulate Flux handle flags', [author], 3),
    ('man3/flux_future_and_then', 'flux_future_or_then', 'functions for sequential composition of futures', [author], 3),
    ('man3/flux_future_and_then', 'flux_future_continue', 'functions for sequential composition of futures', [author], 3),
    ('man3/flux_future_and_then', 'flux_future_continue_error', 'functions for sequential composition of futures', [author], 3),
    ('man3/flux_future_and_then', 'flux_future_and_then', 'functions for sequential composition of futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_fulfill', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_fulfill_error', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_fulfill_with', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_aux_get', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_aux_set', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_set_flux', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_get_flux', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_create', 'flux_future_create', 'support methods for classes that return futures', [author], 3),
    ('man3/flux_future_get', 'flux_future_then', 'synchronize an activity', [author], 3),
    ('man3/flux_future_get', 'flux_future_wait_for', 'synchronize an activity', [author], 3),
    ('man3/flux_future_get', 'flux_future_reset', 'synchronize an activity', [author], 3),
    ('man3/flux_future_get', 'flux_future_destroy', 'synchronize an activity', [author], 3),
    ('man3/flux_future_get', 'flux_future_get', 'synchronize an activity', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_wait_any_create', 'functions for future composition', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_push', 'functions for future composition', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_first_child', 'functions for future composition', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_next_child', 'functions for future composition', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_get_child', 'functions for future composition', [author], 3),
    ('man3/flux_future_wait_all_create', 'flux_future_wait_all_create', 'functions for future composition', [author], 3),
    ('man3/flux_get_rank', 'flux_get_size', 'query Flux broker info', [author], 3),
    ('man3/flux_get_rank', 'flux_get_rank', 'query Flux broker info', [author], 3),
    ('man3/flux_get_reactor', 'flux_set_reactor', 'get/set reactor associated with broker handle', [author], 3),
    ('man3/flux_get_reactor', 'flux_get_reactor', 'get/set reactor associated with broker handle', [author], 3),
    ('man3/flux_handle_watcher_create', 'flux_handle_watcher_get_flux', 'create broker handle watcher', [author], 3),
    ('man3/flux_handle_watcher_create', 'flux_handle_watcher_create', 'create broker handle watcher', [author], 3),
    ('man3/flux_idle_watcher_create', 'flux_prepare_watcher_create', 'create prepare/check/idle watchers', [author], 3),
    ('man3/flux_idle_watcher_create', 'flux_check_watcher_create', 'create prepare/check/idle watchers', [author], 3),
    ('man3/flux_idle_watcher_create', 'flux_idle_watcher_create', 'create prepare/check/idle watchers', [author], 3),
    ('man3/flux_kvs_commit', 'flux_kvs_fence', 'commit a KVS transaction', [author], 3),
    ('man3/flux_kvs_commit', 'flux_kvs_commit_get_treeobj', 'commit a KVS transaction', [author], 3),
    ('man3/flux_kvs_commit', 'flux_kvs_commit_get_sequence', 'commit a KVS transaction', [author], 3),
    ('man3/flux_kvs_commit', 'flux_kvs_commit', 'commit a KVS transaction', [author], 3),
    ('man3/flux_kvs_copy', 'flux_kvs_move', 'copy/move a KVS key', [author], 3),
    ('man3/flux_kvs_copy', 'flux_kvs_copy', 'copy/move a KVS key', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot_get_treeobj', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot_get_blobref', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot_get_sequence', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot_get_owner', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot_cancel', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_getroot', 'flux_kvs_getroot', 'look up KVS root hash', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookupat', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get_unpack', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get_raw', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get_dir', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get_treeobj', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup_get_symlink', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_lookup', 'flux_kvs_lookup', 'look up KVS key', [author], 3),
    ('man3/flux_kvs_namespace_create', 'flux_kvs_namespace_create', 'create/remove a KVS namespace', [author], 3),
    ('man3/flux_kvs_namespace_create', 'flux_kvs_namespace_remove', 'create/remove a KVS namespace', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_destroy', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_put', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_pack', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_vpack', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_mkdir', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_unlink', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_symlink', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_put_raw', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_put_treeobj', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_kvs_txn_create', 'flux_kvs_txn_create', 'operate on a KVS transaction object', [author], 3),
    ('man3/flux_log', 'flux_vlog', 'Log messages to the Flux Message Broker', [author], 3),
    ('man3/flux_log', 'flux_log_set_appname', 'Log messages to the Flux Message Broker', [author], 3),
    ('man3/flux_log', 'flux_log_set_procid', 'Log messages to the Flux Message Broker', [author], 3),
    ('man3/flux_log', 'flux_log', 'Log messages to the Flux Message Broker', [author], 3),
    ('man3/flux_msg_cmp', 'flux_msg_cmp', 'match a message', [author], 3),
    ('man3/flux_msg_create', 'flux_msg_create', 'functions for Flux messages', [author], 3),
    ('man3/flux_msg_create', 'flux_msg_copy', 'functions for Flux messages', [author], 3),
    ('man3/flux_msg_create', 'flux_msg_incref', 'functions for Flux messages', [author], 3),
    ('man3/flux_msg_create', 'flux_msg_decref', 'functions for Flux messages', [author], 3),
    ('man3/flux_msg_create', 'flux_msg_destroy', 'functions for Flux messages', [author], 3),
    ('man3/flux_msg_encode', 'flux_msg_decode', 'convert a Flux message to buffer and back again', [author], 3),
    ('man3/flux_msg_encode', 'flux_msg_encode', 'convert a Flux message to buffer and back again', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_has_flag', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_set_flag', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_clear_flag', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_is_streaming', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_set_streaming', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_is_noresponse', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_set_noresponse', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_is_private', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_has_flag', 'flux_msg_set_private', 'test/set Flux message flags', [author], 3),
    ('man3/flux_msg_handler_addvec', 'flux_msg_handler_delvec', 'bulk add/remove message handlers', [author], 3),
    ('man3/flux_msg_handler_addvec', 'flux_msg_handler_addvec', 'bulk add/remove message handlers', [author], 3),
    ('man3/flux_msg_handler_create', 'flux_msg_handler_destroy', 'manage message handlers', [author], 3),
    ('man3/flux_msg_handler_create', 'flux_msg_handler_start', 'manage message handlers', [author], 3),
    ('man3/flux_msg_handler_create', 'flux_msg_handler_stop', 'manage message handlers', [author], 3),
    ('man3/flux_msg_handler_create', 'flux_msg_handler_create', 'manage message handlers', [author], 3),
    ('man3/flux_open', 'flux_clone', 'open/close connection to Flux Message Broker', [author], 3),
    ('man3/flux_open', 'flux_close', 'open/close connection to Flux Message Broker', [author], 3),
    ('man3/flux_open', 'flux_open', 'open/close connection to Flux Message Broker', [author], 3),
    ('man3/flux_open', 'flux_open_ex', 'open/close connection to Flux Message Broker', [author], 3),
    ('man3/flux_open', 'flux_reconnect', 'open/close connection to Flux Message Broker', [author], 3),
    ('man3/flux_periodic_watcher_create', 'flux_periodic_watcher_reset', 'set/reset a timer', [author], 3),
    ('man3/flux_periodic_watcher_create', 'flux_periodic_watcher_create', 'set/reset a timer', [author], 3),
    ('man3/flux_pollevents', 'flux_pollfd', 'poll Flux broker handle', [author], 3),
    ('man3/flux_pollevents', 'flux_pollevents', 'poll Flux broker handle', [author], 3),
    ('man3/flux_reactor_create', 'flux_reactor_destroy', 'create/destroy/control event reactor object', [author], 3),
    ('man3/flux_reactor_create', 'flux_reactor_run', 'create/destroy/control event reactor object', [author], 3),
    ('man3/flux_reactor_create', 'flux_reactor_stop', 'create/destroy/control event reactor object', [author], 3),
    ('man3/flux_reactor_create', 'flux_reactor_stop_error', 'create/destroy/control event reactor object', [author], 3),
    ('man3/flux_reactor_create', 'flux_reactor_create', 'create/destroy/control event reactor object', [author], 3),
    ('man3/flux_reactor_now', 'flux_reactor_now_update', 'get/update reactor time', [author], 3),
    ('man3/flux_reactor_now', 'flux_reactor_now', 'get/update reactor time', [author], 3),
    ('man3/flux_recv', 'flux_recv', 'receive message using Flux Message Broker', [author], 3),
    ('man3/flux_request_decode', 'flux_request_unpack', 'decode a Flux request message', [author], 3),
    ('man3/flux_request_decode', 'flux_request_decode_raw', 'decode a Flux request message', [author], 3),
    ('man3/flux_request_decode', 'flux_request_decode', 'decode a Flux request message', [author], 3),
    ('man3/flux_request_encode', 'flux_request_encode_raw', 'encode a Flux request message', [author], 3),
    ('man3/flux_request_encode', 'flux_request_encode', 'encode a Flux request message', [author], 3),
    ('man3/flux_requeue', 'flux_requeue', 'requeue a message', [author], 3),
    ('man3/flux_respond', 'flux_respond_pack', 'respond to a request', [author], 3),
    ('man3/flux_respond', 'flux_respond_raw', 'respond to a request', [author], 3),
    ('man3/flux_respond', 'flux_respond_error', 'respond to a request', [author], 3),
    ('man3/flux_respond', 'flux_respond', 'respond to a request', [author], 3),
    ('man3/flux_response_decode', 'flux_response_decode_raw', 'decode a Flux response message', [author], 3),
    ('man3/flux_response_decode', 'flux_response_decode_error', 'decode a Flux response message', [author], 3),
    ('man3/flux_response_decode', 'flux_response_decode', 'decode a Flux response message', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_pack', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_raw', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_message', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_get', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_get_unpack', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_get_raw', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_get_matchtag', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_rpc', 'flux_rpc_get_nodeid', 'perform a remote procedure call to a Flux service', [author], 3),
    ('man3/flux_send', 'flux_send', 'send message using Flux Message Broker', [author], 3),
    ('man3/flux_send', 'flux_send_new', 'send message using Flux Message Broker', [author], 3),
    ('man3/flux_service_register', 'flux_service_register', 'Register service with flux broker', [author], 3),
    ('man3/flux_service_register', 'flux_service_unregister', 'Unregister service with flux broker', [author], 3),
    ('man3/flux_shell_add_completion_ref', 'flux_shell_remove_completion_ref', 'Manipulate conditions for job completion.', [author], 3),
    ('man3/flux_shell_add_completion_ref', 'flux_shell_add_completion_ref', 'Manipulate conditions for job completion.', [author], 3),
    ('man3/flux_shell_add_event_context', 'flux_shell_add_event_context', 'Add context information for standard shell events', [author], 3),
    ('man3/flux_shell_add_event_handler', 'flux_shell_add_event_handler', 'Add an event handler for a shell event', [author], 3),
    ('man3/flux_shell_aux_set', 'flux_shell_aux_get', 'get/set auxiliary handle data', [author], 3),
    ('man3/flux_shell_aux_set', 'flux_shell_aux_set', 'get/set auxiliary handle data', [author], 3),
    ('man3/flux_shell_current_task', 'flux_shell_task_first', 'Get and iterate over shell tasks', [author], 3),
    ('man3/flux_shell_current_task', 'flux_shell_task_next', 'Get and iterate over shell tasks', [author], 3),
    ('man3/flux_shell_current_task', 'flux_shell_current_task', 'Get and iterate over shell tasks', [author], 3),
    ('man3/flux_shell_get_flux', 'flux_shell_get_flux', 'Get a flux_t\* object from flux shell handle', [author], 3),
    ('man3/flux_shell_get_hwloc_xml', 'flux_shell_get_hwloc_xml', 'Access the shell cached copy of local hwloc xml', [author], 3),
    ('man3/flux_shell_get_info', 'flux_shell_info_unpack', 'Manage shell info and rank info', [author], 3),
    ('man3/flux_shell_get_info', 'flux_shell_get_rank_info', 'Manage shell info and rank info', [author], 3),
    ('man3/flux_shell_get_info', 'flux_shell_get_rank_info', 'Manage shell info and rank info', [author], 3),
    ('man3/flux_shell_get_info', 'flux_shell_rank_info_unpack', 'Manage shell info and rank info', [author], 3),
    ('man3/flux_shell_get_info', 'flux_shell_get_info', 'Manage shell info and rank info', [author], 3),
    ('man3/flux_shell_get_jobspec_info', 'flux_shell_jobspec_info_unpack', 'Manage shell jobspec summary information', [author], 3),
    ('man3/flux_shell_get_jobspec_info', 'flux_shell_get_jobspec_info', 'Manage shell jobspec summary information', [author], 3),
    ('man3/flux_shell_get_taskmap', 'flux_shell_get_taskmap', 'Get shell task mapping', [author], 3),
    ('man3/flux_shell_get_hostlist', 'flux_shell_get_hostlist', 'Get the list of hosts in the current job', [author], 3),
    ('man3/flux_shell_getenv', 'flux_shell_get_environ', 'Get and set global environment variables', [author], 3),
    ('man3/flux_shell_getenv', 'flux_shell_setenvf', 'Get and set global environment variables', [author], 3),
    ('man3/flux_shell_getenv', 'flux_shell_unsetenv', 'Get and set global environment variables', [author], 3),
    ('man3/flux_shell_getenv', 'flux_shell_getenv', 'Get and set global environment variables', [author], 3),
    ('man3/flux_shell_getopt', 'flux_shell_getopt_unpack', 'Get and set shell options', [author], 3),
    ('man3/flux_shell_getopt', 'flux_shell_setopt', 'Get and set shell options', [author], 3),
    ('man3/flux_shell_getopt', 'flux_shell_setopt_pack', 'Get and set shell options', [author], 3),
    ('man3/flux_shell_getopt', 'flux_shell_getopt', 'Get and set shell options', [author], 3),
    ('man3/flux_shell_killall', 'flux_shell_killall', 'Send the specified signal to all processes in the shell', [author], 3),
    ('man3/flux_shell_log', 'flux_shell_err', 'Log shell plugin messages to registered shell loggers', [author], 3),
    ('man3/flux_shell_log', 'flux_shell_fatal', 'Log shell plugin messages to registered shell loggers', [author], 3),
    ('man3/flux_shell_log', 'flux_shell_raise', 'Log shell plugin messages to registered shell loggers', [author], 3),
    ('man3/flux_shell_log', 'flux_shell_log_setlevel', 'Log shell plugin messages to registered shell loggers', [author], 3),
    ('man3/flux_shell_log', 'flux_shell_log', 'Log shell plugin messages to registered shell loggers', [author], 3),
    ('man3/flux_shell_plugstack_call', 'flux_shell_plugstack_call', 'Calls the function referenced by topic.', [author], 3),
    ('man3/flux_shell_rpc_pack', 'flux_shell_rpc_pack', 'perform an rpc to a running flux shell using Jansson style pack arguments', [author], 3),
    ('man3/flux_shell_service_register', 'flux_shell_service_register', r'Register a service handler for \`method\` in the shell.', [author], 3),
    ('man3/flux_shell_task_channel_subscribe', 'flux_shell_task_channel_subscribe', r'Call \`cb\` when \`name\` is ready for reading.', [author], 3),
    ('man3/flux_shell_task_get_info', 'flux_shell_task_info_unpack', 'interfaces for fetching task info', [author], 3),
    ('man3/flux_shell_task_get_info', 'flux_shell_task_get_info', 'interfaces for fetching task info', [author], 3),
    ('man3/flux_shell_task_subprocess', 'flux_shell_task_cmd', 'return the subprocess and cmd structure of a shell task, respectively', [author], 3),
    ('man3/flux_shell_task_subprocess', 'flux_shell_task_subprocess', 'return the subprocess and cmd structure of a shell task, respectively', [author], 3),
    ('man3/flux_signal_watcher_create', 'flux_signal_watcher_get_signum', 'create signal watcher', [author], 3),
    ('man3/flux_signal_watcher_create', 'flux_signal_watcher_create', 'create signal watcher', [author], 3),
    ('man3/flux_stat_watcher_create', 'flux_stat_watcher_get_rstat', 'create stat watcher', [author], 3),
    ('man3/flux_stat_watcher_create', 'flux_stat_watcher_create', 'create stat watcher', [author], 3),
    ('man3/flux_timer_watcher_create', 'flux_timer_watcher_reset', 'set/reset a timer', [author], 3),
    ('man3/flux_timer_watcher_create', 'flux_timer_watcher_create', 'set/reset a timer', [author], 3),
    ('man3/flux_watcher_start', 'flux_watcher_stop', 'start/stop/destroy/query reactor watcher', [author], 3),
    ('man3/flux_watcher_start', 'flux_watcher_is_active', 'start/stop/destroy/query reactor watcher', [author], 3),
    ('man3/flux_watcher_start', 'flux_watcher_destroy', 'start/stop/destroy/query reactor watcher', [author], 3),
    ('man3/flux_watcher_start', 'flux_watcher_next_wakeup', 'start/stop/destroy/query reactor watcher', [author], 3),
    ('man3/flux_watcher_start', 'flux_watcher_start', 'start/stop/destroy/query reactor watcher', [author], 3),
    ('man3/flux_watcher_set_priority', 'flux_watcher_set_priority', 'set watcher priority', [author], 3),
    ('man3/hostlist_create', 'hostlist_create', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_destroy', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_decode', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_encode', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_copy', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_append', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_append_list', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_nth', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_find', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_delete', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_count', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_sort', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_uniq', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_first', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_last', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_next', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_current', 'Manipulate lists of hostnames', [author], 3),
    ('man3/hostlist_create', 'hostlist_remove_current', 'Manipulate lists of hostnames', [author], 3),
    ('man3/idset_create', 'idset_create', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_destroy', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_copy', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_set', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_range_set', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_clear', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_range_clear', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_test', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_first', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_next', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_prev', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_empty', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_universe_size', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_last', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_create', 'idset_count', 'Manipulate numerically sorted sets of non-negative integers', [author], 3),
    ('man3/idset_encode','idset_encode', 'Convert idset to string', [author], 3),
    ('man3/idset_decode','idset_decode', 'Convert string to idset', [author], 3),
    ('man3/idset_decode','idset_decode_ex', 'Convert string to idset', [author], 3),
    ('man3/idset_decode','idset_decode_empty', 'Convert string to idset', [author], 3),
    ('man3/idset_decode','idset_decode_info', 'Convert string to idset', [author], 3),
    ('man3/idset_decode','idset_decode_add', 'Convert string to idset', [author], 3),
    ('man3/idset_decode','idset_decode_subtract', 'Convert string to idset', [author], 3),
    ('man3/idset_add', 'idset_equal', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_union', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_add', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_difference', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_subtract', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_intersect', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_has_intersection', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_add','idset_clear_all', 'Perform set operations on idsets', [author], 3),
    ('man3/idset_alloc','idset_alloc', 'Allocate an id from an idset', [author], 3),
    ('man3/idset_alloc','idset_free', 'Allocate an id from an idset', [author], 3),
    ('man3/idset_alloc','idset_free_check', 'Allocate an id from an idset', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_get_flux', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_service_register', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_reprioritize_all', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_reprioritize_job', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_priority_unavail', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_jobtap_get_flux','flux_jobtap_reject_job', 'Flux jobtap plugin interfaces', [author], 3),
    ('man3/flux_sync_create','flux_sync_create', 'Synchronize on system heartbeat', [author], 3),
    ('man3/flux_job_timeleft','flux_job_timeleft', 'Get remaining time for a job', [author], 3),
    ('man5/flux-config', 'flux-config', 'Flux configuration files', [author], 5),
    ('man5/flux-config-access', 'flux-config-access', 'configure Flux instance access', [author], 5),
    ('man5/flux-config-bootstrap', 'flux-config-bootstrap', 'configure Flux instance bootstrap', [author], 5),
    ('man5/flux-config-tbon', 'flux-config-tbon', 'configure Flux overlay network', [author], 5),
    ('man5/flux-config-exec', 'flux-config-exec', 'configure Flux job execution service', [author], 5),
    ('man5/flux-config-systemd', 'flux-config-systemd', 'configure Flux systemd support', [author], 5),
    ('man5/flux-config-ingest', 'flux-config-ingest', 'configure Flux job ingest service', [author], 5),
    ('man5/flux-config-resource', 'flux-config-resource', 'configure Flux resource service', [author], 5),
    ('man5/flux-config-policy', 'flux-config-policy', 'configure Flux job policy', [author], 5),
    ('man5/flux-config-queues', 'flux-config-queues', 'configure Flux job queues', [author], 5),
    ('man5/flux-config-job-manager', 'flux-config-job-manager', 'configure Flux job manager service', [author], 5),
    ('man5/flux-config-kvs', 'flux-config-kvs', 'configure Flux kvs service', [author], 5),
    ('man7/flux-broker-attributes', 'flux-broker-attributes', 'overview Flux broker attributes', [author], 7),
    ('man7/flux-jobtap-plugins', 'flux-jobtap-plugins', 'overview Flux jobtap plugin API', [author], 7),
    ('man7/flux-environment', 'flux-environment', 'Flux environment overview', [author], 7),
]
