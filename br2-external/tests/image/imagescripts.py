# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The shell scripts of the image and the programs they run.

The scripts run on the board under the BusyBox of the image, which holds the
applets of ``busybox.config`` and no more. The tests of the scripts run on a
build machine with a GNU userland, where a script that calls ``chgrp`` passes
whether or not the image has it. So the scripts are read instead: ``uses``
lists every program or builtin that one runs, and two checks put the list
against the image.

* ``test_scripts.py`` puts it against the configuration of BusyBox, the
  install lines of the packages and the table of the builtins of ash, with no
  build;
* ``check-applets`` puts it against the built tree, and runs ``type`` in the
  image's own shell for the builtins and ``--help`` of each applet for the
  options that the scripts give it.

A command word that holds an expansion (``"$DROP"``) cannot be read; DYNAMIC
names, for each one, the programs it stands for. A program that a script starts
for another (``onu-svc start NAME -- PROGRAM``) is a use as well.
"""

import os
import re
from collections import namedtuple

import shellwords

HERE = os.path.dirname(os.path.realpath(__file__))
BR2 = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(BR2)
BOARD = os.path.join(BR2, "board", "dfp-34x-2c3")
OVERLAY = os.path.join(BOARD, "overlay")
PACKAGES = os.path.join(BR2, "package")

# What one command of a script is: the script's name, its line, the program
# (as the script spells it, with its directory if it has one), the name
# without it, and the options it is given.
Use = namedtuple("Use", "script line spelled name options")

# Programs that start the program that follows their own options: where it is.
# onu-svc and svc_start (a function of onu-apply that calls it) take it after
# "--"; onu-drop takes a user and then the program, after its own -c CAPS.
LAUNCHERS = {"onu-svc": "--", "svc_start": "--", "onu-drop": "user"}

# The command words that hold an expansion, by script, and the programs that
# each stands for. test_scripts.py reads the scripts for the reason.
DYNAMIC = {
    # DROP=$LIB/onu-drop
    ("onu-run", '"$DROP"'): ("onu-drop",),
    # prog is the first word of what verbs() echoes for a change on trial.
    ("onu-change", '"$prog"'): ("onu-lane", "onu-mgmt"),
    ("onu-change", '"${v%% *}"'): ("onu-lane", "onu-mgmt"),
    # CALL=$ROOT/usr/lib/onu/onu-call
    ("onu-dhcpc", '"$CALL"'): ("onu-call",),
}

# What ash holds whatever the configuration, and what it holds with a symbol
# of busybox.config. check-applets runs "type" in the image's ash to confirm.
ASH_BUILTINS = {".", ":", "break", "cd", "continue", "eval", "exec", "exit", "export", "false", "kill",
                "local", "pwd", "read", "readonly", "return", "set", "shift", "times", "trap", "true",
                "type", "umask", "unset", "wait"}
ASH_GATED = {"echo": "ASH_ECHO", "printf": "ASH_PRINTF", "test": "ASH_TEST", "[": "ASH_TEST",
             "getopts": "ASH_GETOPTS", "command": "ASH_CMDCMD", "alias": "ASH_ALIAS"}

# Words after a dash that name a signal, which kill and killall take as an option.
SIGNALS = {"HUP", "INT", "QUIT", "KILL", "TERM", "USR1", "USR2", "CONT", "STOP"}

# The applets whose symbol is not the upper-case name.
APPLET_SYMBOL = {"[": "TEST"}

# Programs of the packages that no install line of this tree names: those of
# Buildroot's own package, and the ones that a CMake install puts in place. The
# symbol is what the defconfig has to turn on for the package.
PACKAGE_PROGRAMS = {"BR2_PACKAGE_DROPBEAR": ("dropbear", "dropbearkey"),
                    "BR2_PACKAGE_NETIFD": ("netifd",)}


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def sources():
    """The scripts of the image as (the name it is told by, its file in the tree).

    The overlay's scripts, and the ones that onu-utils installs. The boot script
    is also linked as /etc/rc.d/S10boot; it is listed once.
    """
    found = [("rcS", os.path.join(OVERLAY, "etc", "init.d", "rcS")),
             ("init", os.path.join(OVERLAY, "sbin", "init")),
             ("board.sh", os.path.join(OVERLAY, "etc", "profile.d", "board.sh"))]
    scripts = os.path.join(REPO, "onu-utils", "scripts")
    mk = read(os.path.join(PACKAGES, "onu-utils", "onu-utils.mk"))
    for name in re.findall(r"\$\(@D\)/scripts/(\S+) \$\(TARGET_DIR\)/", mk):
        found.append((os.path.basename(name), os.path.join(scripts, name)))
    return found


def tree_scripts(target):
    """The shell scripts of a built target tree as (the name it is told by, its file).

    Every regular file that starts with a shell's shebang, and the files that
    /etc/profile reads, which have none.
    """
    found = []
    for root, _, names in sorted(os.walk(target)):
        for name in sorted(names):
            path = os.path.join(root, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            relative = os.path.relpath(path, target)
            with open(path, "rb") as handle:
                head = handle.read(10)
            if head.startswith((b"#!/bin/sh", b"#!/bin/ash")) or relative == "etc/profile" \
                    or relative.startswith("etc/profile.d/"):
                found.append((name, path))
    return found


def installed():
    """The programs that the packages of this tree install, by name, from their .mk files."""
    names = set()
    for package in sorted(os.listdir(PACKAGES)):
        directory = os.path.join(PACKAGES, package)
        for entry in sorted(os.listdir(directory)) if os.path.isdir(directory) else ():
            if not entry.endswith(".mk"):
                continue
            for line in read(os.path.join(directory, entry)).splitlines():
                match = re.search(r"\$\(INSTALL\) .*-m 0755 \S+ \$\(TARGET_DIR\)/(\S+)$", line)
                if match:
                    names.add(os.path.basename(match.group(1)))
                match = re.search(r"\bln -sf? \S+ \$\(TARGET_DIR\)/(\S+)$", line)
                if match:
                    names.add(os.path.basename(match.group(1)))
    return names


def enabled():
    """The symbols of busybox.config that are on, without the CONFIG_."""
    symbols = set()
    for line in read(os.path.join(BOARD, "busybox.config")).splitlines():
        match = re.match(r"^CONFIG_([A-Z0-9_]+)=y$", line)
        if match:
            symbols.add(match.group(1))
    return symbols


def applet_symbol(name):
    return APPLET_SYMBOL.get(name, name.upper().replace("-", "_"))


def launched(command, name):
    """What a launcher starts: (its own arguments, the program, the program's arguments), or None.

    name is the launcher's program; command holds its arguments. None is for a
    program that is no word to read (an expansion) or is not given.
    """
    args = command.args
    if LAUNCHERS[name] == "--":
        if "--" not in args:
            return None
        own, rest = args[:args.index("--")], args[args.index("--") + 1:]
    else:
        own = args[:2] if args[:1] == ["-c"] else []
        rest = args[len(own) + 1:]  # after the user to run as
    if not rest or "$" in rest[0] or "`" in rest[0]:
        return None
    return own, re.sub(r"""['"\\]""", "", rest[0]), rest[1:]


def uses(script, text):
    """Every program or builtin that a script runs: (list of Use, list of the words left unread).

    Functions of the script itself are not uses. A word that holds an expansion
    is a use of each program that DYNAMIC lists for it, and a word that the
    list does not know is left in the second list as (line, text).
    """
    found = []
    unread = []
    commands, functions = shellwords.commands(text)
    for command in commands:
        if command.dynamic:
            names = DYNAMIC.get((script, command.name))
            if names is None:
                unread.append((command.line, command.name))
                continue
        else:
            names = (command.name,)
        for spelled in names:
            name = os.path.basename(spelled)
            started = launched(command, name) if name in LAUNCHERS else None
            if name not in functions:
                found.append(Use(script, command.line, spelled, name,
                                 words_options(started[0] if started else command.args)))
            if started:
                program = started[1]
                found.append(Use(script, command.line, program, os.path.basename(program),
                                 words_options(started[2])))
    return found, unread


def words_options(args):
    """The options among the arguments of a command: the words that are a dash and a letter.

    Short options as written, a cluster (``-sf``) as the letters it holds, a
    long option whole. A number after the dash (``-1``) is no option.
    """
    found = []
    for arg in args:
        match = re.match(r"^--[A-Za-z][A-Za-z0-9-]*$|^-([A-Za-z]+)", arg)
        if not match:
            continue
        if arg.startswith("--"):
            found.append(arg)
        elif arg[1:] not in SIGNALS:
            found.extend("-" + letter for letter in match.group(1))
    return sorted(set(found))
