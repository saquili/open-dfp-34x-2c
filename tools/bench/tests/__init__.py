# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Unit tests of the bench; they need no board and no network.

Run them from the ``tools`` directory::

    python3 -m unittest discover -s bench/tests -t .

or from the repository root with ``-s tools/bench/tests -t tools``, or with
``python3 -m bench selftest``. The tests drive the tool against a simulated
console and a scripted boot loader on a virtual clock; a test that starts a
real tmux, ssh or other host-control process, that sleeps for real, or that
touches a network address other than the loopback, fails at once.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

# Programs that act on real hardware or hosts; no test may start one.
_FORBIDDEN_PROGRAMS = frozenset({"tmux", "ssh", "scp", "sudo", "nft", "ss", "ip", "ifconfig"})
_LOOPBACK = frozenset({"127.0.0.1", "::1", "localhost"})
_installed = False


def _program(args: object) -> str:
    first = args if isinstance(args, (str, bytes, os.PathLike)) else (args[0] if args else "")  # type: ignore[index]
    text = os.fsdecode(first)
    return os.path.basename(text.split()[0]) if text.split() else ""


def _install_guards() -> None:
    """Make a test that reaches for real hardware or a real network fail."""
    global _installed
    if _installed:
        return
    _installed = True
    real_init = subprocess.Popen.__init__

    def checked_init(self, args, *rest, **kwargs):  # type: ignore[no-untyped-def]
        program = _program(args)
        if program in _FORBIDDEN_PROGRAMS:
            raise AssertionError("a test started the host program %r" % program)
        real_init(self, args, *rest, **kwargs)

    subprocess.Popen.__init__ = checked_init  # type: ignore[method-assign]

    real_sleep = time.sleep

    def checked_sleep(seconds: float) -> None:
        if seconds > 0.25:
            raise AssertionError("a test slept for real for %.2f s; use the virtual clock"
                                 % seconds)
        real_sleep(seconds)

    time.sleep = checked_sleep  # type: ignore[assignment]

    def loopback_only(real):  # type: ignore[no-untyped-def]
        def checked(self, *args, **kwargs):  # type: ignore[no-untyped-def]
            address = args[-1] if args else None
            if isinstance(address, tuple) and address[0] not in _LOOPBACK:
                raise AssertionError("a test used the network address %r" % (address,))
            return real(self, *args, **kwargs)
        return checked

    for name in ("connect", "connect_ex", "sendto", "bind"):
        setattr(socket.socket, name, loopback_only(getattr(socket.socket, name)))


_install_guards()
