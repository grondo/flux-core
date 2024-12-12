#############################################################
# Copyright 2024 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################

import concurrent.futures
import copy
import glob
import os
import sys
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from subprocess import Popen

import flux
import flux.importer
from flux.idset import IDset
from flux.utils import tomli as tomllib
from flux.utils.graphlib import TopologicalSorter


class RankConditional:
    """Conditional rank statement, e.g. ``>0``"""

    def __init__(self, arg):
        if arg[0] == ">":
            self.gt = True
        elif arg[0] == "<":
            self.gt = False
        else:
            raise ValueError("rank condition must be either < or >")
        self.rank = int(arg[1:])

    def test(self, rank):
        if self.gt:
            return rank > self.rank
        return rank < self.rank


class RankIDset:
    """Rank by IDset, e.g. ``all`` or ``0-1``"""

    def __init__(self, arg):
        self.ranks = None
        self.all = False
        if arg == "all":
            self.all = True
        else:
            self.ranks = IDset(arg)

    def test(self, rank):
        if self.all:
            return True
        return self.ranks[rank]


def rank_conditional(arg):
    """Rank conditional factory function"""
    cls = RankIDset
    if arg.startswith((">", "<")):
        cls = RankConditional
    return cls(arg)


class Task:

    VALID_ARGS = {
        "ranks": "all",
        "provides": [],
        "requires": [],
        "needs": [],
        "before": [],
        "after": [],
        "requires_attrs": [],
        "requires_config": [],
        "disabled": False,
    }

    def __init__(self, name, *args, **kwargs):
        self.name = name
        self.starttime = None
        self.endtime = None

        for attr in kwargs:
            if attr not in self.VALID_ARGS:
                raise ValueError(f"{self.name}: unknown task argument {attr}")

        for attr, default in self.VALID_ARGS.items():
            val = kwargs.get(attr, default)
            # Handle case where attr is set explicitly to None, in which case,
            # inherit the default
            if val is None:
                val = default
            setattr(self, attr, val)

        # convert self.ranks to rank conditional object:
        self.ranks = rank_conditional(self.ranks)

    def enabled(self, context):
        if self.disabled or not self.ranks.test(context.rank):
            return False
        for key in self.requires_config:
            val = context.handle.conf_get(key)
            if not val:
                return False
        for attr in self.requires_attrs:
            val = context.attr_get(attr)
            if not val:
                return False
        return True

    def runtask(self, context):
        self.starttime = time.time()
        try:
            self.run(context)
        finally:
            self.endtime = time.time()

    def run(self, context):
        print(self.name)


class CodeTask(Task):

    def __init__(self, name, func, *args, **kwargs):
        self.func = func
        super().__init__(name, *args, **kwargs)

    def run(self, context):
        self.func(context)


class Module(Task):

    VALID_KEYS = (
        "name",
        "args",
        "ranks",
        "provides",
        "requires",
        "needs",
        "before",
        "after",
        "requires-attrs",
        "requires-config",
    )

    def __init__(self, conf):
        if "name" not in conf:
            raise ValueError("Missing required config key 'name'")

        self.name = conf["name"]
        self.args = conf.get("args", [])
        self.run = self.load

        # Build kwargs to pass along to Task class
        kwargs = {}
        for key, val in conf.items():
            if key not in self.VALID_KEYS:
                raise ValueError(f"{self.name}: invalid config key {key}")
            key = key.replace("-", "_")
            if key in super().VALID_ARGS:
                kwargs[key] = val

        super().__init__(self.name, **kwargs)

    def set_remove(self):
        """Mark module to be removed instead of loaded (the default)"""
        # swap before and after
        self.after, self.before = self.before, self.after
        # XXX: remove needs and requires since these do not apply to
        # module removal:
        self.needs = []
        self.requires = []
        self.run = self.remove

    def load(self, context):
        self.args.extend(context.getopts(self.name, also=self.provides))
        name = (
            "FLUX_MODPROBE_MODULE_"
            + self.name.replace("-", "_").upper()
            + "_ARGS_APPEND"
        )
        if name in os.environ:
            self.args.extend(os.environ[name].split(","))
        payload = {"path": self.name, "args": self.args}

        # print(f"module.load {payload}")
        context.handle.rpc("module.load", payload).get()

    def remove(self, context):
        try:
            # print(f"module.remove {self.name}")
            context.handle.rpc("module.remove", {"name": self.name}).get()
        except FileNotFoundError:
            # Ignore already unloaded modules
            pass

    def __repr__(self):
        if self.disabled:
            return f"{self.name}.disabled"
        elif self.run == self.load:
            return f"{self.name}.load"
        return f"{self.name}.remove"


class TaskDB:

    def __init__(self):
        self.tasks = defaultdict(list)

    def add(self, task):
        """Add a task to the TaskDB"""
        for name in task.provides:
            self.tasks[name].append(task)
        self.tasks[task.name].append(task)

    def get(self, service):
        """Get the current task providing 'service'"""
        try:
            task = self.tasks[service][-1]
            return task
        except IndexError:
            raise ValueError(f"no such task or module {service}") from None

    def set_alternative(self, service, name):
        """Select a specific alternative 'name' for service"""
        if name is None:
            self.disable(service)
            return
        lst = self.tasks[service]
        try:
            index = next(i for i, x in enumerate(lst) if x.name == name)
        except StopIteration:
            raise ValueError(f"no module {name} provides {service}")
        lst.append(lst.pop(index))

    def disable(self, service):
        """disable task/module/service 'service'"""
        for task in self.tasks[service]:
            task.disabled = True

    def any_provides(self, tasks, name):
        """Return True if any task in tasks provides name"""
        for task in [self.get(x) for x in tasks]:
            if not task.disabled and name in (task.name, *task.provides):
                return True
        return False


def task(name, **kwargs):
    """
    Decorator for rc1/rc3 task functions.

    This decorator is applied to functions in an rc1 or rc3 python
    source file to turn them into valid flux-modprobe(1) tasks.

    Args:
    name (required, str): The name of this task.
    ranks (required, str): A rank expression that indicates on which
        ranks this task should be invoked. ``ranks`` may be a valid
        RFC 22 Idset string, a single integer prefixed with ``<`` or
        ``<`` to indicate matching ranks less than or greater than a
        given rank, or the string ``all`` (the default if ``ranks``
        is not specified). Examples: ``0``, ``>0``, ``0-3``.
    requires (options, list): An optional list of task or module names
        this tasnk requires. This is used to ensure required tasks are
        active when activating another task. It does not indicate that
        this task will necessarily be run before the tasks it requires.
        (See ``before`` for that feature)
    needs (options, list): Disable this task if any task in ``needs`` is
        not active.
    provides (optional, list): An optional list of string service name
        that this task provides. This can be used to set up alternatives
        for a given service. (Mostly useful with modules)
    before (optional, list): A list of tasks or modules for which this task
        must be run before.
    after (optional, list) A list of tasks or modules for which this task
        must be run after.
    requires_attrs (optional, list): A list of broker attributes on which
        this task depends. If any of the attributes are not set then the
        task will not be run.
    requires_config (optional, list): A list of config keys on which this
        task depends. If any of the specified config keys are not set,
        then this task will not be run.

    Example:
    ::
        # Declare a task that will be run after the kvs module is loaded
        # only on rank 0
        @task("test", ranks="0", after=["kvs"])
        def test_kvs_task(context):
            # do something with kvs
    """

    def create_task(func):
        return CodeTask(name, func, **kwargs)

    return create_task


class Context:

    tls = threading.local()

    def __init__(self, modprobe):
        self.modprobe = modprobe
        self._data = {}
        self._args = {}
        self.module_args = defaultdict(list)

    @property
    def handle(self):
        """Return a per-thread handle created on demand"""
        if not hasattr(self.tls, "_handle"):
            self.tls._handle = flux.Flux()
        return self.tls._handle

    @property
    def rank(self):
        return self.handle.get_rank()

    def set(self, key, value):
        """Set arbitrary data at key for future use. (see get())"""
        self._data[key] = value

    def get(self, key, default=None):
        """Get arbitrary data set by other tasks with optional default value"""
        return self._data.get(key, default)

    def attr_get(self, attr, default=None):
        """Get broker attribute with optional default"""
        try:
            return self.handle.attr_get(attr)
        except FileNotFoundError:
            return default

    def conf_get(self, key, default=None):
        """Get config key with optional default"""
        return self.handle.conf_get(key, default=default)

    def rpc(self, topic, *args, **kwargs):
        """Convenience function to call context.handle.rpc()"""
        return self.handle.rpc(topic, *args, **kwargs)

    def setopt(self, module, option):
        """Append option to module opts"""
        self.module_args[module].append(option)

    def getopts(self, name, also=None):
        """Get module opts for module 'name'
        If also is provided, append any module options for those names as well
        """
        lst = [name]
        if also is not None:
            lst.extend(also)
        result = []
        for name in lst:
            result.extend(self.module_args[name])
        return result

    def bash(self, command):
        """Execute command under ``bash -c``"""
        Popen(["bash", "-c", command]).wait()

    def load_modules(self, modules):
        """Set a list of modules to load by name"""
        self.modprobe.activate_modules(modules)

    def remove_modules(self, modules=None):
        """
        Set a list of modules to remove by name.
        Remove all if ``modules`` is None.
        """
        self.modprobe.set_remove(modules)

    def set_alternative(self, name, alternative):
        """Force an alternative for module ``name`` to ``alternative``"""
        self.modprobe.taskdb.set_alternative(name, alternative)


class ModuleList:
    """Simple class for iteration and lookup of loaded modules"""

    def __init__(self, handle):
        resp = handle.rpc("module.list").get()
        self.loaded_modules = []
        self.servicemap = {}
        for entry in resp["mods"]:
            self.loaded_modules.append(entry["name"])
            for name in entry["services"]:
                self.servicemap[name] = entry["name"]

    def __iter__(self):
        for name in self.loaded_modules:
            yield name

    def lookup(self, name):
        return self.servicemap.get(name, None)


class Modprobe:

    def __init__(self, confdir=None, timing=False):
        self.exitcode = 0
        self.timing = None
        self.t0 = None
        self._locals = None

        self.taskdb = TaskDB()
        self.context = Context(self)
        self.handle = self.context.handle
        self.rank = self.handle.get_rank()

        self.confdir = confdir

        # Active tasks are those added via the @task decorator, and
        # which will be active by default when running "all" tasks:
        self._active_tasks = []

        if timing:
            self.timing = []
            self.t0 = time.time()

    @property
    def timestamp(self):
        if not self.t0:
            return 0.0
        return time.time() - self.t0

    @property
    def active_tasks(self):
        return [
            name
            for name in self.process_needs(self._active_tasks)
            if self.get_task(name).enabled(self.context)
        ]

    def add_timing(self, name, starttime, end=None):
        if self.timing is None:
            return
        if end is None:
            end = self.timestamp
        self.timing.append(
            {"name": name, "starttime": starttime, "duration": end - starttime}
        )

    def save_task_timing(self, tasks):
        if self.timing is None:
            return
        for task in sorted(tasks, key=lambda x: x.starttime):
            self.add_timing(
                task.name,
                starttime=task.starttime - self.t0,
                end=task.endtime - self.t0,
            )

    def add_task(self, task):
        """Add a task to internal task db"""
        self.taskdb.add(task)

    def add_active_task(self, task):
        """Add a task to the task db and active tasks list"""
        self.add_task(task)
        self._active_tasks.append(task.name)

    def get_task(self, name, default=None):
        """Return current task providing string 'name'"""
        return self.taskdb.get(name)

    def has_task(self, name):
        """Return True if task exists in taskdb"""
        try:
            self.taskdb.get(name)
            return True
        except ValueError:
            return False

    def add_modules(self, file):
        with open(file, "rb") as fp:
            config = tomllib.load(fp)
        for entry in config["modules"]:
            self.add_task(Module(entry))

    def configure_modules(self, path=None):
        if path is None:
            etc = Path(self.confdir)
            path = etc / "modules.toml"
        else:
            etc = Path(path).parent

        self.add_modules(path)
        dirs = ["{etc}/modules.d/*.toml"]
        dirs.extend(
            filter(
                lambda s: not s.isspace(),
                os.environ.get("FLUX_MODPROBE_PATH", "").split(":"),
            )
        )
        for directory in dirs:
            if Path(directory).exists():
                for file in glob.glob(f"{directory}/modules.d/*.toml"):
                    self.add_modules(file)

    def solve_tasks_recursive(self, tasks, visited=None, skipped=None):
        """Recursively find all requirements of 'tasks'"""

        if visited is None:
            visited = set()
        if skipped is None:
            skipped = set()
        result = set()
        to_visit = [x for x in tasks if x not in visited]

        for name in to_visit:
            task = self.get_task(name)
            if task.enabled(self.context):
                result.add(task.name)
            else:
                skipped.add(task.name)
            visited.add(task.name)
            if task.requires:
                rset = self.solve_tasks_recursive(
                    tasks=task.requires, visited=visited, skipped=skipped
                )
                result.update(rset)

        return result

    def process_needs(self, tasks):
        """Remove all tasks in tasks where task.needs is not met"""
        for name in tasks.copy():
            task = self.get_task(name)
            for need in task.needs:
                if not self.taskdb.any_provides(tasks, need):
                    if name in tasks:
                        tasks.remove(name)
        return tasks

    def solve(self, tasks):
        t0 = self.timestamp
        result = self.solve_tasks_recursive(tasks)
        self.add_timing("solve", t0)
        return result

    def process_before(self, tasks, deps):
        """Process any task.before by appending this task's name to all
        successor's predecessor list.
        """

        def deps_add_all(name):
            """Add name as a predecessor to all entries in deps"""
            for task in [self.get_task(x) for x in deps.keys()]:
                if "*" not in task.before:
                    deps[task.name].append(name)

        for name in tasks:
            task = self.get_task(name)
            for successor in task.before:
                if successor == "*":
                    deps_add_all(task.name)
                else:
                    # resolve real successor name:
                    successor = self.get_task(successor).name
                    if successor in deps:
                        deps[successor].append(task.name)

    def get_deps(self, tasks, reverse=False):
        """Return dependencies for tasks as dict of names to predecessor list"""
        t0 = self.timestamp
        deps = {}
        for name in tasks:
            task = self.get_task(name)
            if "*" in task.after:
                deps[task.name] = [
                    self.get_task(x).name for x in tasks if x != task.name
                ]
            else:
                after_tasks = [self.get_task(x).name for x in task.after]
                deps[task.name] = [x for x in after_tasks if x in tasks]
        self.process_before(tasks, deps)
        self.add_timing("deps", t0)

        return deps

    def run(self, deps):
        t0 = self.timestamp
        sorter = TopologicalSorter(deps)
        sorter.prepare()
        self.add_timing("prepare", t0)

        max_workers = None
        if sys.version_info < (3, 8):
            # In Python < 3.8, idle threads are not reused up to
            # max_workers. For these versions, set a low max_workers
            # to force thread (and therefore Flux handle) reuse:
            max_workers = 5

        executor = ThreadPoolExecutor(max_workers=max_workers)
        futures = {}
        started = {}

        while sorter.is_active():
            for task in [self.get_task(x) for x in sorter.get_ready()]:
                if task.name not in started:
                    future = executor.submit(task.runtask, self.context)
                    started[task.name] = task
                    futures[future] = task

            done, not_done = concurrent.futures.wait(
                futures.keys(), return_when=concurrent.futures.FIRST_COMPLETED
            )

            for future in done:
                task = futures[future]
                try:
                    future.result()
                except Exception as exc:
                    print(f"{task.name}: {exc}", file=sys.stderr)
                    self.exitcode = 1
                sorter.done(task.name)
                del futures[future]

        self.save_task_timing(started.values())

        return self.exitcode

    def remove_modules(self, modules):
        return self.set_remove(modules)

    def _load_file(self, path):
        module = flux.importer.import_path(path)
        tasks = filter(lambda x: isinstance(x, CodeTask), vars(module).values())
        for task in tasks:
            self.add_active_task(task)

        # Check for function setup() which should run before all other tasks
        setup = getattr(module, "setup", None)
        if callable(setup):
            setup(self.context)

    def read_rcfile(self, name):
        # For absolute file path, just add tasks from single file:
        if name.endswith(".py"):
            self._load_file(name)
            return

        # O/w, add tasks from "named" file in confdir, plus all tasks
        # in {confdir}/{name}.d/*.py
        etc = Path(self.confdir)
        path = etc / f"{name}.py"
        self._load_file(path)
        for file in glob.glob(f"{etc}/{name}.d/*.py"):
            self._load_file(file)

    def activate_modules(self, modules):
        for module in modules:
            task = self.get_task(module)
            if not isinstance(task, Module):
                raise ValueError(f"{module} is not a module")
            self._active_tasks.append(module)

    def load(self, modules):
        """Load modules and their dependencies"""
        self.run(self.get_deps(self.solve(modules)))

    def solve_modules_remove(self, modules=None):
        """Solve for a set of currently loaded modules to remove"""
        mlist = ModuleList(self.handle)
        all_modules = [x for x in mlist if self.has_task(x)]

        if not modules:
            # remove all configured modules
            modules = all_modules
        else:
            # Check if all specified modules are loaded:
            for module in modules:
                if not mlist.lookup(module):
                    raise ValueError(f"module {module} is not loaded")

        # Calculate reverse deps for all loaded modules
        deps = self.get_deps(all_modules)
        rdeps = defaultdict(set)

        for name, deplist in deps.items():
            for mod in deplist:
                rdeps[mlist.lookup(mod)].add(name)

        # Determine if removal of modules will cause any other modules
        # to become "unused", i.e. not needed by other modules
        rdeps_copy = copy.deepcopy(rdeps)

        def remove_dep(name):
            for modname, dep_set in rdeps_copy.items():
                if not dep_set:
                    continue
                dep_set.discard(name)
                if not dep_set:
                    remove_dep(modname)
                    if modname in mlist:
                        modules.append(modname)

        for name in modules:
            remove_dep(mlist.lookup(name))

        # Raise an error if any module in modules has rdeps remaining
        for name in modules:
            if rdeps_copy[name]:
                raise ValueError(
                    f"{name} still in use by " + ", ".join(rdeps_copy[name])
                )

        tasks = set()
        for service in modules:
            name = mlist.lookup(service)
            if name is not None:
                tasks.add(name)

        deps = {}
        for name in tasks:
            deps[name] = {x for x in rdeps[name] if x in tasks}

        return list(tasks), deps

    def set_remove(self, modules=None):
        if modules is None:
            mlist = ModuleList(self.handle)
            modules = [x for x in mlist if self.has_task(x)]

        for module in modules:
            task = self.get_task(module)
            task.set_remove()
            self.add_active_task(task)

    def remove(self, modules):
        """Remove loaded modules"""
        tasks, deps = self.solve_modules_remove(modules)
        [self.get_task(x).set_remove() for x in deps.keys()]
        self.run(deps)
