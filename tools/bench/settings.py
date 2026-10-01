# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The configuration of the bench: every value has a default and an override.

Settings come from three places, each over the one before: the defaults below,
a TOML file (``--config FILE`` or the ``BENCH_CONFIG`` environment variable),
and ``--set section.key=value`` on the command line. A value that is not
TOML (an ssh destination, say) is taken as text. An unknown key or a value of
the wrong type is an error before anything is touched.

The addresses are the documentation ranges, so a bench that has not been
configured cannot reach a real host by accident. The console of the second
UART (session ``uart-wifi-2``), which is wired to the reference board, is
read-only unless a setting says otherwise; the console of the board under
development is writable.
"""

from __future__ import annotations

import os
import tomllib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from . import image
from .console import BenchError, ConsoleConfig
from .flash import FlashConfig
from .host import BoardAccess, HostConfig
from .ramboot import RamBootConfig


class SettingsError(BenchError):
    """A setting is unknown or has the wrong type."""

    exit_status = 2


# key -> (type, default); a default of None means "derived", see ``load``.
SCHEMA: dict[str, tuple[type, object]] = {
    "console.session": (str, "uart-wifi-1"),
    "console.transcript": (str, ""),
    "console.tmux_socket": (str, ""),
    "console.read_only": (bool, None),
    "console.bridge_host": (str, ""),
    "console.bridge_port": (int, 2001),
    "console.serial_device": (str, ""),
    "host.ssh": (str, "local"),
    "host.ssh_options": (list, []),
    "host.served_dir": (str, "/tmp/bench/images"),
    "host.work_dir": (str, "/tmp/bench"),
    "host.interface": (str, "eth1"),
    "host.bind": (str, "192.0.2.1"),
    "host.port": (int, 69),
    "host.rule_tag": (str, "bench-tftp"),
    "host.sudo": (bool, True),
    "board.control_address": (str, "198.51.100.2"),
    "board.data_address": (str, "192.0.2.2"),
    "board.user": (str, "root"),
    "board.jump": (str, ""),
    "board.jump_options": (list, []),
    "board.loader_ipaddr": (str, "192.0.2.2"),
    "board.loader_serverip": (str, "192.0.2.1"),
    "board.loader_netmask": (str, "255.255.255.252"),
    "image.kernel_name": (str, "uImage"),
    "image.rootfs_name": (str, "rootfs"),
    "image.squashfs_compressions": (list, [image.SQUASHFS_GZIP, image.SQUASHFS_LZ4]),
    "boot.ready_pattern": (str, "login:"),
    "boot.ready_limit": (float, 60.0),
    "boot.recovery": (str, "fuse"),
    "log.path": (str, ""),
}
READ_ONLY_SESSIONS = frozenset({"uart-wifi-2"})


@dataclass
class Settings:
    """The configuration, as the pieces the package takes."""

    console: ConsoleConfig
    bridge_host: str
    bridge_port: int
    serial_device: str
    host: HostConfig
    access: BoardAccess
    ramboot: RamBootConfig
    flash: FlashConfig
    log_path: str


def _coerce(key: str, value: object, wanted: type) -> object:
    if wanted is float and isinstance(value, int) and not isinstance(value, bool):
        return float(value)
    if not isinstance(value, wanted) or (wanted is int and isinstance(value, bool)):
        raise SettingsError("%s must be %s, not %r" % (key, wanted.__name__, value))
    return value


def _parse_value(text: str) -> object:
    try:
        return tomllib.loads("v = " + text)["v"]
    except tomllib.TOMLDecodeError:
        return text


def collect(config_path: str | None = None, overrides: Sequence[str] = (),
            environ: Mapping[str, str] | None = None) -> dict[str, object]:
    """The setting values: defaults, then the file, then the overrides."""
    environ = os.environ if environ is None else environ
    values = {key: default for key, (_type, default) in SCHEMA.items()}
    path = config_path or environ.get("BENCH_CONFIG")
    if path:
        try:
            with open(path, "rb") as handle:
                loaded = tomllib.load(handle)
        except OSError as exc:
            raise SettingsError("cannot read the configuration %s: %s" % (path, exc.strerror)) from exc
        except tomllib.TOMLDecodeError as exc:
            raise SettingsError("the configuration %s is not valid TOML: %s" % (path, exc)) from exc
        for section, table in loaded.items():
            if not isinstance(table, dict):
                raise SettingsError("%s must be a table" % section)
            for name, value in table.items():
                _set(values, "%s.%s" % (section, name), value)
    for item in overrides:
        key, sep, text = item.partition("=")
        if not sep:
            raise SettingsError("--set takes KEY=VALUE, not %r" % item)
        _set(values, key, _parse_value(text))
    return values


def _set(values: dict[str, object], key: str, value: object) -> None:
    if key not in SCHEMA:
        raise SettingsError("unknown setting %r; the settings are: %s"
                            % (key, ", ".join(sorted(SCHEMA))))
    wanted = SCHEMA[key][0]
    values[key] = _coerce(key, value, wanted)


def load(config_path: str | None = None, overrides: Sequence[str] = (),
         environ: Mapping[str, str] | None = None) -> Settings:
    """Build the configuration the package takes."""
    v = collect(config_path, overrides, environ)
    session = str(v["console.session"])
    read_only = v["console.read_only"]
    if read_only is None:
        read_only = session in READ_ONLY_SESSIONS  # the reference board's console
    console = ConsoleConfig(session=session, transcript=str(v["console.transcript"]),
                            tmux_socket=str(v["console.tmux_socket"]), read_only=bool(read_only))
    host = HostConfig(ssh=str(v["host.ssh"]), ssh_options=tuple(map(str, v["host.ssh_options"])),  # type: ignore[arg-type]
                      served_dir=str(v["host.served_dir"]), work_dir=str(v["host.work_dir"]),
                      interface=str(v["host.interface"]), bind=str(v["host.bind"]),
                      port=int(v["host.port"]),  # type: ignore[call-overload]
                      rule_tag=str(v["host.rule_tag"]), sudo=bool(v["host.sudo"]))
    access = BoardAccess(control_address=str(v["board.control_address"]),
                         data_address=str(v["board.data_address"]), user=str(v["board.user"]),
                         jump=str(v["board.jump"]),
                         jump_options=tuple(map(str, v["board.jump_options"])))  # type: ignore[arg-type]
    boot = RamBootConfig(
        image_name=str(v["image.kernel_name"]), expect_ipaddr=str(v["board.loader_ipaddr"]),
        expect_serverip=str(v["board.loader_serverip"]),
        expect_netmask=str(v["board.loader_netmask"]), ready_pattern=str(v["boot.ready_pattern"]),
        ready_limit=float(v["boot.ready_limit"]),  # type: ignore[arg-type]
        recovery=str(v["boot.recovery"]))
    if boot.recovery not in ("fuse", "reset"):
        raise SettingsError("boot.recovery is 'fuse' or 'reset', not %r" % boot.recovery)
    flash = FlashConfig(
        kernel_name=str(v["image.kernel_name"]), rootfs_name=str(v["image.rootfs_name"]),
        squashfs_compressions=tuple(int(c) for c in v["image.squashfs_compressions"]),  # type: ignore[attr-defined]
        boot=boot, bridge_host=str(v["console.bridge_host"]),
        bridge_port=int(v["console.bridge_port"]))  # type: ignore[call-overload]
    return Settings(console=console, bridge_host=flash.bridge_host,
                    bridge_port=flash.bridge_port, serial_device=str(v["console.serial_device"]),
                    host=host, access=access, ramboot=boot, flash=flash,
                    log_path=str(v["log.path"]))


def describe() -> list[str]:
    """One line per setting: its key, type and default, for the usage text."""
    lines = []
    for key, (kind, default) in SCHEMA.items():
        shown = "derived from the session" if default is None else repr(default)
        lines.append("%-30s %-6s %s" % (key, kind.__name__, shown))
    return lines
