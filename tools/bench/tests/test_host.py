# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""The image host and the board's ssh lines, against a command-runner double."""

from __future__ import annotations

import os
import shlex
import signal
import subprocess
import sys
from unittest import mock

from bench import host as hostmod
from bench.host import (BoardAccess, HostConfig, HostError, ImageHost, RunResult,
                        SubprocessRunner, board_command)

from .simclock import VirtualClock
from .simhost import Rule, SimImageHost, SimRunner
from .support import TempDirTestCase


class HostCase(TempDirTestCase):

    ssh = "bench@image-host.example"

    def setUp(self) -> None:
        super().setUp()
        self.clock = VirtualClock()
        self.state = SimImageHost()
        self.runner = SimRunner(self.state, self.clock)
        self.config = HostConfig(ssh=self.ssh, ssh_options=("-o", "ProxyJump=jump.example"),
                                 served_dir="/tmp/bench/images", work_dir="/tmp/bench",
                                 interface="eth1", bind="192.0.2.1", rule_tag="bench-tftp")
        self.host = ImageHost(self.config, self.runner, clock=self.clock)
        self.image = os.path.join(self.tmp, "uImage")
        with open(self.image, "wb") as handle:
            handle.write(b"I" * 5000)
        self.server_source = os.path.join(self.tmp, "tftp.py")
        with open(self.server_source, "w", encoding="utf-8") as handle:
            handle.write("# the server\n")

    def remote_programs(self) -> list[str]:
        return self.runner.programs()


class Commands(HostCase):

    def test_a_remote_command_goes_through_ssh_without_a_remote_command_or_a_tty(self) -> None:
        line = self.host.command(["stat", "-c", "%s", "/tmp/a b"])
        self.assertEqual(line[:5], ["ssh", "-o", "RemoteCommand=none", "-o", "RequestTTY=no"])
        self.assertIn("ProxyJump=jump.example", line)
        self.assertEqual(line[-2], self.ssh)
        self.assertEqual(shlex.split(line[-1]), ["stat", "-c", "%s", "/tmp/a b"])

    def test_privileged_commands_use_sudo_without_a_password_prompt(self) -> None:
        line = self.host.command(["nft", "list", "ruleset"], privileged=True)
        self.assertEqual(shlex.split(line[-1])[:3], ["sudo", "-n", "nft"])
        no_sudo = ImageHost(HostConfig(ssh="local", sudo=False), self.runner)
        self.assertEqual(no_sudo.command(["nft"], privileged=True), ["nft"])

    def test_local_commands_run_directly(self) -> None:
        local = ImageHost(HostConfig(ssh="local"), self.runner)
        self.assertEqual(local.command(["ss", "-lun"]), ["ss", "-lun"])
        self.assertEqual(local.command(["ss"], privileged=True), ["sudo", "-n", "ss"])

    def test_the_configuration_is_validated(self) -> None:
        bad = [dict(served_dir="relative"), dict(served_dir="/tmp/a b"),
               dict(work_dir="/tmp/../etc"), dict(interface="eth0; reboot"),
               dict(rule_tag='x"y'), dict(bind="not-an-address"), dict(bind="192.0.2.1;")]
        for change in bad:
            with self.subTest(change=change), self.assertRaises(HostError):
                ImageHost(HostConfig(**change), self.runner)


class Pushing(HostCase):

    def test_push_is_atomic(self) -> None:
        size = self.host.push_image(self.image, "uImage")
        self.assertEqual(size, 5000)
        self.assertEqual(self.state.files["/tmp/bench/images/uImage"], b"I" * 5000)
        self.assertNotIn("/tmp/bench/images/uImage.part", self.state.files)
        scp = [c for c in self.runner.calls if c[0] == "scp"]
        self.assertEqual(len(scp), 1)
        self.assertTrue(scp[0][-1].endswith(":/tmp/bench/images/uImage.part"), scp[0])
        self.assertEqual(scp[0][:2], ["scp", "-q"])
        self.assertEqual(scp[0][2:6], ["-o", "RemoteCommand=none", "-o", "RequestTTY=no"])
        self.assertEqual(self.remote_programs(), ["mkdir", "scp", "mv", "stat"])
        move = shlex.split(self.runner.calls[2][-1])
        self.assertEqual(move, ["mv", "-f", "/tmp/bench/images/uImage.part",
                                "/tmp/bench/images/uImage"])

    def test_a_stale_file_of_the_same_name_is_replaced(self) -> None:
        self.state.files["/tmp/bench/images/uImage"] = b"old"
        self.host.push_image(self.image, "uImage")
        self.assertEqual(self.state.files["/tmp/bench/images/uImage"], b"I" * 5000)

    def test_a_size_that_differs_is_an_error(self) -> None:
        original = self.runner._scp

        def short(line):
            result = original(line)
            self.state.files[line[-1].split(":", 1)[1]] = b"short"
            return result

        self.runner._scp = short  # type: ignore[method-assign]
        with self.assertRaises(HostError) as caught:
            self.host.push_image(self.image, "uImage")
        self.assertIn("5 bytes on the image host, 5000 here", str(caught.exception))

    def test_a_failed_copy_is_an_error_and_nothing_is_renamed(self) -> None:
        self.state.scp_fails = True
        with self.assertRaises(HostError):
            self.host.push_image(self.image, "uImage")
        self.assertNotIn("mv", self.remote_programs())

    def test_only_plain_names_are_pushed(self) -> None:
        for name in ("../x", "a/b", ".h", "", "a b", "x" * 65):
            with self.subTest(name=name), self.assertRaises(HostError):
                self.host.push_image(self.image, name)
        self.assertEqual(self.runner.calls, [])

    def test_a_local_push_is_a_copy_and_a_rename(self) -> None:
        served = os.path.join(self.tmp, "served")
        local = ImageHost(HostConfig(ssh="local", served_dir=served,
                                     work_dir=self.tmp.rstrip("/")), self.runner)
        self.assertEqual(local.push_image(self.image, "uImage"), 5000)
        self.assertEqual(sorted(os.listdir(served)), ["uImage"])
        with open(os.path.join(served, "uImage"), "rb") as handle:
            self.assertEqual(handle.read(), b"I" * 5000)
        self.assertEqual(self.runner.calls, [])

    def test_file_size(self) -> None:
        self.assertIsNone(self.host.file_size("/tmp/bench/images/uImage"))
        self.host.push_image(self.image, "uImage")
        self.assertEqual(self.host.file_size("/tmp/bench/images/uImage"), 5000)


class Serving(HostCase):

    def test_the_sockets_are_asked_never_the_process_list(self) -> None:
        self.assertFalse(self.host.listening())
        self.state.listening[69] = "ours"
        self.assertTrue(self.host.listening())
        self.assertTrue(all("pgrep" not in " ".join(call) for call in self.runner.calls))
        self.assertEqual(set(self.remote_programs()), {"ss"})

    def test_a_port_that_merely_ends_in_69_is_not_the_tftp_port(self) -> None:
        self.state.listening[8069] = "foreign"
        self.assertFalse(self.host.listening())

    def test_start_pushes_the_source_stops_the_old_server_and_starts_a_new_one(self) -> None:
        self.state.listening[69] = "ours"
        self.host.start_server(self.server_source)
        self.assertEqual(self.state.files["/tmp/bench/tftp.py"], b"# the server\n")
        self.assertEqual(len(self.state.launched), 1)
        script = self.state.launched[0]
        self.assertIn("nohup", script)
        self.assertIn("--bind 192.0.2.1", script)
        self.assertIn("--dir /tmp/bench/images", script)
        self.assertIn("--port 69", script)
        self.assertIn("--pidfile /tmp/bench/tftp.pid", script)
        self.assertIn(">> /tmp/bench/tftp.log 2>&1 < /dev/null &", script)
        self.assertNotIn("0.0.0.0", script)
        self.assertIn("kill", " ".join(" ".join(c) for c in self.runner.calls))
        for call in self.runner.calls:
            if "nohup" in call[-1]:
                self.assertEqual(shlex.split(call[-1])[:3], ["sudo", "-n", "sh"])

    def test_something_else_on_the_port_is_never_stopped(self) -> None:
        self.state.listening[69] = "foreign"
        with self.assertRaises(HostError) as caught:
            self.host.start_server(self.server_source)
        self.assertIn("will not stop it", str(caught.exception))
        self.assertEqual(self.state.launched, [])

    def test_a_server_that_does_not_come_up_is_reported_with_its_log(self) -> None:
        self.state.start_never_listens = True
        with self.assertRaises(HostError) as caught:
            self.host.start_server(self.server_source)
        self.assertIn("/tmp/bench/tftp.log", str(caught.exception))
        self.assertAlmostEqual(self.clock.slept, 5.0 + 0.3, delta=0.6)

    def test_stop_removes_only_the_server_the_tool_started(self) -> None:
        self.state.listening[69] = "ours"
        self.host.stop_server()
        self.assertFalse(self.host.listening())
        self.state.listening[69] = "foreign"
        self.host.stop_server()
        self.assertTrue(self.host.listening())


class StopScript(TempDirTestCase):
    """The script that stops the server, run for real in a plain shell.

    A pid file that outlives its process names a number that may belong to
    something else; only a process whose command line names the server's own
    script may be signalled.
    """

    def setUp(self) -> None:
        super().setUp()
        config = HostConfig(ssh="local", work_dir=self.tmp)
        self.host = ImageHost(config, SimRunner(SimImageHost(), VirtualClock()))
        self.pidfile = os.path.join(self.tmp, "tftp.pid")

    def start(self, *extra: str) -> subprocess.Popen:
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)", *extra])
        self.addCleanup(process.wait)
        self.addCleanup(process.kill)
        with open(self.pidfile, "w", encoding="ascii") as handle:
            handle.write("%d\n" % process.pid)
        return process

    def stop(self) -> None:
        subprocess.run(["sh", "-c", self.host.stop_script()], check=True, timeout=20)

    def test_a_process_that_is_not_the_server_is_left_alone(self) -> None:
        stranger = self.start()
        self.stop()
        self.assertIsNone(stranger.poll())
        self.assertFalse(os.path.exists(self.pidfile))

    def test_a_process_that_names_another_script_is_left_alone(self) -> None:
        stranger = self.start(os.path.join(self.tmp, "other.py"))
        self.stop()
        self.assertIsNone(stranger.poll())

    def test_the_server_is_stopped_and_its_pid_file_removed(self) -> None:
        server = self.start(os.path.join(self.tmp, "tftp.py"))
        self.stop()
        self.assertEqual(server.wait(timeout=10), -signal.SIGTERM)
        self.assertFalse(os.path.exists(self.pidfile))

    def test_an_empty_or_missing_pid_file_does_nothing(self) -> None:
        self.stop()
        with open(self.pidfile, "w", encoding="ascii") as handle:
            handle.write("\n")
        self.stop()
        self.assertFalse(os.path.exists(self.pidfile))


class Firewall(HostCase):

    def test_firewall_rule_idempotent(self) -> None:
        self.assertEqual(self.host.rule_present(), "absent")
        self.assertEqual(self.host.ensure_rule(), "added")
        self.assertEqual(self.host.ensure_rule(), "present")
        self.assertEqual(self.host.ensure_rule(), "present")
        self.assertEqual(len(self.state.rules), 1, "never added twice")
        rule = self.state.rules[0]
        self.assertEqual((rule.interface, rule.port, rule.tag), ("eth1", 69, "bench-tftp"))
        adds = [c for c in self.runner.calls if "add" in shlex.split(c[-1])]
        self.assertEqual(len(adds), 1)
        self.assertEqual(shlex.split(adds[0][-1]),
                         ["sudo", "-n", "nft", "add", "rule", "inet", "filter", "input",
                          "iifname", '"eth1"', "udp", "dport", "69", "accept", "comment",
                          '"bench-tftp"'])

    def test_a_failure_to_add_is_reported_not_raised(self) -> None:
        self.state.nft_add_fails = True
        outcome = self.host.ensure_rule()
        self.assertTrue(outcome.startswith("failed: "), outcome)
        self.assertIn("No such file or directory", outcome)

    def test_a_host_without_the_chain(self) -> None:
        self.state.chain_exists = False
        self.assertEqual(self.host.rule_present(), "unknown")
        self.assertTrue(self.host.ensure_rule().startswith("failed"))

    def test_the_rule_is_removed_by_its_comment(self) -> None:
        self.host.ensure_rule()
        self.state.rules.append(Rule(99, "eth9", 22, "someone-elses"))
        self.assertEqual(self.host.remove_rule(), 1)
        self.assertEqual([r.tag for r in self.state.rules], ["someone-elses"])
        self.assertEqual(self.host.remove_rule(), 0)

    def test_the_comment_is_matched_whole(self) -> None:
        self.state.rules.append(Rule(5, "eth1", 69, "bench-tftp-old"))
        self.assertEqual(self.host.rule_present(), "absent")
        self.assertEqual(self.host.remove_rule(), 0)

    def test_nothing_touches_the_interface_or_other_rules(self) -> None:
        """Only list, add the one rule, and delete by handle: no link or address changes."""
        self.host.prepare(self.image, "uImage", self.server_source)
        self.host.remove_rule()
        forbidden = ("ip", "ifconfig", "systemctl", "service", "iptables", "ifup", "ifdown",
                     "nmcli", "networkctl", "ethtool", "route", "reboot")
        for call in self.runner.calls:
            words = shlex.split(call[-1]) if call[0] == "ssh" else call
            if words[:2] == ["sudo", "-n"]:
                words = words[2:]
            self.assertNotIn(words[0], forbidden, call)
            if words[0] == "nft":
                self.assertTrue(words[1:5] in (["list", "chain", "inet", "filter"],
                                               ["-a", "list", "chain", "inet"],
                                               ["add", "rule", "inet", "filter"],
                                               ["delete", "rule", "inet", "filter"]), words)
            if words[0] == "sh":
                for word in forbidden:
                    self.assertNotIn(" %s " % word, words[2])


class Preparing(HostCase):

    def test_prepare_makes_the_three_things_true(self) -> None:
        notes = self.host.prepare(self.image, "uImage", self.server_source)
        self.assertEqual(notes, [])
        status = self.host.check("uImage")
        self.assertEqual((status.file_size, status.listening, status.rule), (5000, True, "present"))
        self.assertEqual(status.problems("uImage"), [])

    def test_the_source_is_pushed_at_every_run(self) -> None:
        self.host.prepare(self.image, "uImage", self.server_source)
        with open(self.server_source, "w", encoding="utf-8") as handle:
            handle.write("# a newer server\n")
        self.host.prepare(self.image, "uImage", self.server_source)
        self.assertEqual(self.state.files["/tmp/bench/tftp.py"], b"# a newer server\n")
        self.assertEqual(len(self.state.launched), 2)

    def test_a_missing_rule_is_a_note_not_an_error(self) -> None:
        self.state.nft_add_fails = True
        notes = self.host.prepare(self.image, "uImage", self.server_source)
        self.assertEqual(len(notes), 1)
        self.assertIn("allowance", notes[0])

    def test_check_names_each_missing_thing(self) -> None:
        status = self.host.check("uImage")
        self.assertEqual(status.problems("uImage"),
                         ["the served file uImage does not exist on the image host",
                          "nothing listens on the TFTP port of the image host",
                          "the firewall allowance for UDP 69 is absent"])
        self.state.chain_exists = False
        self.assertEqual(len(self.host.check("uImage").problems("uImage")), 2,
                         "an unknown firewall state is not reported as absent")


class BoardLines(TempDirTestCase):

    def test_a_direct_line_has_the_restart_options(self) -> None:
        access = BoardAccess(control_address="198.51.100.2", data_address="192.0.2.2")
        line = board_command(access, access.control_address, hostmod.SYSRQ_RESTART)
        self.assertEqual(line[0], "ssh")
        for pair in ("ConnectTimeout=6", "BatchMode=yes", "ServerAliveInterval=3",
                     "ServerAliveCountMax=2", "StrictHostKeyChecking=no",
                     "UserKnownHostsFile=/dev/null", "LogLevel=ERROR", "RemoteCommand=none",
                     "RequestTTY=no"):
            self.assertIn(pair, line)
        self.assertEqual(line[-2:], ["root@198.51.100.2", "echo b > /proc/sysrq-trigger"])
        self.assertFalse(any(word.startswith("ProxyCommand") for word in line))

    def test_through_a_jump_host_the_proxy_command_forwards_with_w(self) -> None:
        access = BoardAccess(jump="bench@image-host.example", jump_options=("-p", "2222"))
        line = board_command(access, "192.0.2.2", "true")
        proxy = next(word for word in line if word.startswith("ProxyCommand="))
        self.assertIn("-W %h:%p", proxy)
        self.assertIn("RemoteCommand=none", proxy)
        self.assertIn("RequestTTY=no", proxy)
        self.assertIn("-p 2222", proxy)
        self.assertTrue(proxy.endswith("bench@image-host.example"))
        self.assertNotIn("-J", line)

    def test_the_addresses_are_tried_control_first(self) -> None:
        self.assertEqual(BoardAccess().addresses(), ["198.51.100.2", "192.0.2.2"])


class Subprocess(TempDirTestCase):

    def test_the_runner_runs_a_program(self) -> None:
        result = SubprocessRunner().run([sys.executable, "-c",
                                         "import sys; print('out'); sys.stderr.write('err'); "
                                         "sys.exit(3)"])
        self.assertEqual(result, RunResult(3, "out\n", "err"))

    def test_a_missing_program_and_a_timeout_are_results_not_exceptions(self) -> None:
        self.assertEqual(SubprocessRunner().run(["no-such-program-here"]).returncode, 127)
        with mock.patch("subprocess.run", side_effect=subprocess.TimeoutExpired("x", 1)):
            self.assertEqual(SubprocessRunner().run(["x"], timeout=1).returncode, 124)

    def test_spawn_starts_a_process_without_waiting(self) -> None:
        proc = SubprocessRunner().spawn([sys.executable, "-c", "pass"])
        proc.wait(timeout=10)
        self.assertEqual(proc.poll(), 0)
