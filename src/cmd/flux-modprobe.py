#############################################################
# Copyright 2024 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################

import argparse
import concurrent.futures
import json
import os
import sys
import threading
import time
from collections import ChainMap, defaultdict
from concurrent.futures import ThreadPoolExecutor
from itertools import chain
from pathlib import Path
from subprocess import Popen

import flux
import flux.kvs
from flux.idset import IDset
from flux.utils import tomli as tomllib
from flux.utils.graphlib import TopologicalSorter


class RankConditional:
    """Conditional rank statement, e.g. '>0'"""

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
    """Rank IDset statement, e.g. "all" or "0-1" """

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


class TLSFlux:
    """On demand, per-thread, singleton Flux handle"""

    tls = threading.local()

    def __new__(cls):
        if not hasattr(cls.tls, "_handle"):
            cls.tls._handle = flux.Flux()
        return cls.tls._handle


class Task:
    """Base 'Task' class"""

    def __init__(self, details, runlevel=None):
        self.name = details["name"]
        self.runlevel = runlevel

        ranks = details.get("ranks", "all")
        if ranks.startswith((">", "<")):
            self.ranks = RankConditional(ranks)
        else:
            self.ranks = RankIDset(ranks)

        self.depends = details.get("depends", [])
        self.after = details.get("after", [])
        self.requires_config = details.get("requires-config", [])
        self.requires_attrs = details.get("requires-attrs", [])
        self.uses_attrs = details.get("uses-attrs", [])
        self.provides = details.get("provides", [])

        self.broker_config = {}
        self.broker_attrs = {}

    @property
    def handle(self):
        """Return the thread-local singleton FLux handle"""
        return TLSFlux()

    @property
    def runtime(self):
        self.endtime - self.starttime

    def runtask(self):
        self.starttime = time.time()
        self.run()
        self.endtime = time.time()

    def check_runlevel(self, runlevel=None):
        """For runlevel-specific tasks, return true if runlevel matches"""
        if self.runlevel is not None:
            if runlevel is None:
                return False
            return int(self.runlevel) == int(runlevel)
        return True

    def check_conf(self, handle):
        for key in self.requires_config:
            val = handle.conf_get(key)
            if not val:
                return False
            self.broker_config[key] = val
        return True

    def check_attrs(self, handle):
        for attr in self.requires_attrs:
            try:
                self.broker_attrs[attr] = handle.attr_get(attr)
            except FileNotFoundError:
                return False
        for attr in self.uses_attrs:
            try:
                self.broker_attrs[attr] = handle.attr_get(attr)
            except FileNotFoundError:
                pass
        return True


class CodeTask(Task):
    """A task which runs a snippet of python code via exec()

    Broker config and attribute keys listed in requires-config,
    requires-attrs, and uses-attrs will be made available to the code
    in the local dictionaries 'conf' and 'attrs'.
    """

    def __init__(self, details, runlevel):
        self.proc = None
        self.code = details.get("code")
        super().__init__(details, runlevel)

    def run(self):
        exec(
            self.code,
            globals(),
            {"conf": self.broker_config, "attrs": self.broker_attrs},
        )


class ModuleLoadTask(Task):
    """A task that loads a named module"""

    def __init__(self, details):
        self.args = details.get("args", [])
        super().__init__(details)

    def run(self):
        self.handle.rpc("module.load", {"path": self.name, "args": self.args}).get()


class ModuleRemoveTask(ModuleLoadTask):
    """A task that removes a named module"""

    def run(self):
        try:
            self.handle.rpc("module.remove", {"name": self.name}).get()
        except FileNotFoundError:
            # Ignore missing modules when removing them
            pass


class ShellTask(Task):
    """A task that runs a snippet of shell script using bash(1)"""

    def __init__(self, details, runlevel):
        self.command = details.get("command")
        super().__init__(details, runlevel)

    def run(self):
        Popen(["bash", "-c", self.command]).wait()


def create_task(details, remove=False, runlevel=None):
    if "code" in details:
        return CodeTask(details, runlevel=runlevel)
    elif "command" in details:
        return ShellTask(details, runlevel=runlevel)
    else:
        if remove:
            return ModuleRemoveTask(details)
        else:
            return ModuleLoadTask(details)


class Modprobe:
    def __init__(self, conf=None, remove=False, handle=None):
        self.t0 = time.time()
        if handle is None:
            handle = TLSFlux()
        self.handle = handle
        self.rank = handle.get_rank()
        self.exitcode = 0
        self.sorter = None
        self.timing = None
        self.tasks = {}
        self.deps = {}
        self.alternatives = defaultdict(list)
        self.reverse_dependencies = remove

        if "FLUX_MODPROBE_TIMING" in os.environ:
            self.timing = []

        if conf is None:
            t0 = self.timestamp
            conf = self.read_config()
            self.add_timing("read-config", t0)

        t0 = self.timestamp
        self.modules = {}
        for entry in conf["modules"]:
            task = create_task(entry, remove=remove)
            if task.provides:
                for name in task.provides:
                    self.alternatives[name].append(task)
            else:
                self.modules[task.name] = task

        # Build the set of rc1/rc3 tasks:
        self._rc_tasks = {}
        for i in (1, 3):
            self._rc_tasks[i] = {
                t["name"]: create_task(t, runlevel=i) for t in conf[f"rc{i}"]["tasks"]
            }
        self.add_timing("build-tasks", t0)

    def read_config(self, path="modprobe.toml"):
        etc_path = Path(self.handle.attr_get("broker.rc1_path")).parent
        path = etc_path / "modprobe.toml"
        with open(path, "rb") as fp:
            return tomllib.load(fp)

    def add_timing(self, name, starttime, endtime=None):
        if self.timing is None:
            return
        if endtime is None:
            endtime = self.timestamp
        self.timing.append(
            {"name": name, "starttime": starttime, "duration": endtime - starttime}
        )

    @property
    def timestamp(self):
        return time.time() - self.t0

    def _task_enabled(self, task, runlevel=None):
        return all(
            (
                task.ranks.test(self.rank),
                task.check_conf(self.handle),
                task.check_attrs(self.handle),
                task.check_runlevel(runlevel),
            )
        )

    def rc_tasks(self, runlevel=None):
        if runlevel is None:
            return ChainMap(self._rc_tasks[1], self._rc_tasks[3])
        return self._rc_tasks[int(runlevel)]

    def _reverse_deps(self, deps, tasks, skipped, runlevel=None):
        """Reverse a depednency graph for use in unloading modules/rc3"""

        # reverse dependencies by switching predecessor and successor
        # Note: there's probably a _much_ better way to accomplish this:
        new_deps = defaultdict(set)
        for name, deplist in deps.items():
            new_deps[name] = set()
            for task in deplist:
                new_deps[task].add(name)

        if runlevel == 3:
            #  rc3 tasks run after all modules unloaded unless otherwise
            #  specified via depends.
            all_modules = {x for x in self.modules.keys() if x in tasks}
            for task in self.rc_tasks(3).values():
                if task.name not in deps:
                    # This task is skipped
                    continue
                if task.after:
                    #  Add any 'after' modules/tasks to predecessor set:
                    after = {x for x in task.after if x in deps}
                    new_deps[task.name].update(after)
                else:
                    #  O/w, all modules must be removed before this task:
                    new_deps[task.name].update(all_modules)

        return new_deps

    def solve_all(self, runlevel=None):
        """Return dependency graph for all tasks in runlevel"""
        tasks = {}
        deps = {}
        skipped = set()

        #  Get highest priority alternatives:
        alternatives = [x.pop() for x in self.alternatives.values()]

        #  Create current task list from modules, tasks, and alternatives
        for task in chain(
            self.modules.values(), self.rc_tasks().values(), alternatives
        ):
            if self._task_enabled(task, runlevel=runlevel):
                tasks[task.name] = task
            else:
                skipped.add(task.name)

        if runlevel == 3:
            # add all rc1 tasks to skip list
            for task in self.rc_tasks(1).values():
                skipped.add(task.name)

        for task in tasks.values():
            deps[task.name] = {x for x in task.depends if x not in skipped}
            # Check all deps are valid:
            for dep in deps[task.name]:
                if dep not in tasks:
                    raise ValueError(f"{task.name}: invalid depends: {dep}")

        if self.reverse_dependencies:
            deps = self._reverse_deps(deps, tasks, skipped, runlevel)
        return tasks, deps

    def _solve_tasks_recursive(self, tasks, all_tasks, visited, skipped, runlevel=None):
        """
        Return a dictionary of task name to Task object for all tasks
        in tasks and their dependencies recursively.
        Args:
            all_tasks (dict): All configured tasks
            visited (set): set of already visited tasks
            skipped (set): set of skipped tasks
        """
        task_dict = {}
        to_visit = [x for x in tasks if x not in visited]
        for name in to_visit:
            try:
                task = all_tasks[name]
            except KeyError:
                raise ValueError(f"no such task or module {name}") from None
            if self._task_enabled(task, runlevel=runlevel):
                task_dict[name] = task
            else:
                skipped.add(name)
            visited.add(name)
            if task.depends:
                task_dict.update(
                    self._solve_tasks_recursive(
                        tasks=task.depends,
                        all_tasks=all_tasks,
                        visited=visited,
                        skipped=skipped,
                        runlevel=runlevel,
                    )
                )
        return task_dict

    def solve(self, tasks=None, runlevel=None):
        """Return a dependency graph for one or more tasks

        If no tasks specified, then the dependency graph for all tasks at
        the current runlevel will be returned
        """
        if not tasks:
            return self.solve_all(runlevel=runlevel)

        visited = set()
        skipped = set()

        # Include names of all alternatives in case they are included
        #  explicitly:
        other = {}
        for lst in self.alternatives.values():
            other.update({x.name: x for x in lst})

        # Get a map of highest priority alternatives:
        alternatives = {
            name: self.alternatives[name].pop() for name in self.alternatives.keys()
        }

        # Combine modules, current runlevel tasks, and alternatives into a map:
        all_tasks = ChainMap(self.modules, self.rc_tasks(), alternatives, other)

        tasks = self._solve_tasks_recursive(
            tasks, all_tasks, visited, skipped, runlevel=runlevel
        )

        deps = {}
        for task in tasks.values():
            deps[task.name] = {x for x in task.depends if x not in skipped}

        if self.reverse_dependencies:
            deps = self._reverse_deps(deps, tasks, skipped, runlevel)
        return tasks, deps

    def prep(self, tasks=None, runlevel=None):
        self.tasks, deps = self.solve(tasks=tasks, runlevel=runlevel)
        self.sorter = TopologicalSorter(deps)
        self.sorter.prepare()

    def get_batch_list(self):
        batches = []
        while self.sorter.is_active():
            ready = self.sorter.get_ready()
            batches.append(ready)
            self.sorter.done(*ready)
        return batches

    def run(self, runlevel=None):
        """Run all configured tasks in order until done"""

        t0 = self.timestamp
        self.prep(runlevel=runlevel)
        self.add_timing("prepare", t0)

        #  Start a threadpool executor so that tasks can wait in the
        #  background:
        executor = ThreadPoolExecutor(max_workers=6)
        futures = {}
        started = set()
        while self.sorter.is_active():
            ready = self.sorter.get_ready()
            tasks = [self.tasks[x] for x in ready]

            #  Run all tasks that are ready, and wait for them to finish
            #  in the background via concurrent.futures
            for task in tasks:
                #  Ensure tasks with aliases are started only once
                if task.name not in started:
                    # print(f"starting {task.name}")
                    future = executor.submit(task.runtask)
                    started.add(task.name)
                    futures[future] = task

            done, not_done = concurrent.futures.wait(
                futures.keys(), return_when=concurrent.futures.FIRST_COMPLETED
            )

            #  Mark all completed tasks as done. Next loop iteration will then
            #  return an newly ready tasks:
            #
            for future in done:
                task = futures[future]
                try:
                    future.result(timeout=0)
                except Exception as exc:
                    print(f"{task.name}: {exc}", file=sys.stderr)
                    self.exitcode = 1
                # print(f"{task.name} done")
                self.sorter.done(task.name)
                del futures[future]

        if self.timing is not None and runlevel == 1 and self.rank == 0:
            for name, task in sorted(
                self.tasks.items(), key=lambda item: item[1].starttime
            ):
                self.add_timing(
                    task.name, task.starttime - self.t0, task.endtime - self.t0
                )
            flux.kvs.put(self.handle, "modprobe.stats", self.timing)
            flux.kvs.commit(self.handle)

        return self.exitcode


def rc1(args):
    sys.exit(Modprobe().run(runlevel=1))


def rc3(args):
    sys.exit(Modprobe(remove=True).run(runlevel=3))


def show_tasks(args):
    remove = args.rc == 3
    M = Modprobe(remove=remove)
    M.prep(tasks=args.tasks, runlevel=args.rc)
    print(json.dumps(M.get_batch_list()))


def parse_args():
    parser = argparse.ArgumentParser(prog="flux-modprobe")
    subparsers = parser.add_subparsers(
        title="subcommands", description="", dest="subcommand"
    )
    subparsers.required = True

    rc1_parser = subparsers.add_parser(
        "rc1", formatter_class=flux.util.help_formatter()
    )
    rc1_parser.set_defaults(func=rc1)

    rc3_parser = subparsers.add_parser(
        "rc3", formatter_class=flux.util.help_formatter()
    )
    rc3_parser.set_defaults(func=rc3)

    show_tasks_parser = subparsers.add_parser(
        "show-tasks", formatter_class=flux.util.help_formatter()
    )
    show_tasks_parser.add_argument(
        "--rc",
        type=int,
        default=None,
        metavar="1|3",
        help="Specify rc1 or rc3. Default is neither",
    )
    show_tasks_parser.add_argument(
        "tasks",
        type=str,
        metavar="MODULES|TASKS",
        nargs="*",
        help="Specify modules/tasks to show. Default: all",
    )
    show_tasks_parser.set_defaults(func=show_tasks)

    return parser.parse_args()


@flux.util.CLIMain()
def main():
    args = parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

# vi: ts=4 sw=4 expandtab
