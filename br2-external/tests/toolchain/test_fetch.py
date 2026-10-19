# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of toolchain/fetch: the pin, the checks of a file, the unpacking, without the network."""

import hashlib
import importlib.machinery
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import urllib.request

HERE = os.path.dirname(os.path.realpath(__file__))
EXTERNAL = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(EXTERNAL, "toolchain", "fetch")
PIN = os.path.join(EXTERNAL, "toolchain", "PIN")


def load():
    loader = importlib.machinery.SourceFileLoader("toolchain_fetch", SCRIPT)
    spec = importlib.util.spec_from_loader("toolchain_fetch", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


FETCH = load()


class Pin(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)

    def write(self, text):
        path = os.path.join(self.dir, "PIN")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def run_fetch(self, *args, env=None):
        return subprocess.run([sys.executable, "-I", SCRIPT, *args], capture_output=True,
                              text=True, env=env)

    def test_the_repository_pin_names_every_key(self):
        pin = FETCH.read_pin(PIN)
        self.assertEqual(sorted(pin), sorted(FETCH.KEYS))
        self.assertEqual(pin["TARBALL"], "open-dfp-34x-2c-toolchain.tar.gz")

    def test_a_pin_without_a_tag_names_no_release_and_prints_nothing(self):
        path = self.write("".join("%s=\n" % key for key in FETCH.KEYS))
        done = self.run_fetch("--pin", path, "--dl", self.dir, "--out", self.dir)
        self.assertEqual((done.returncode, done.stdout), (0, ""))

    def test_a_pin_without_a_key_cannot_be_used(self):
        path = self.write("TAG=\n")
        done = self.run_fetch("--pin", path)
        self.assertEqual(done.returncode, 2)
        self.assertIn("no TARBALL", done.stderr)

    def test_a_line_that_is_no_key_is_refused(self):
        path = self.write("".join("%s=\n" % key for key in FETCH.KEYS) + "URL=x\n")
        self.assertEqual(self.run_fetch("--pin", path).returncode, 2)

    def test_a_pinned_release_needs_sizes_and_digests(self):
        lines = {key: "" for key in FETCH.KEYS}
        lines.update(TAG="toolchain-1", TARBALL="tc.tar.gz", SOURCES="s.tar",
                     TARBALL_SIZE="10", SOURCES_SIZE="10", TARBALL_SHA256="0" * 64, SOURCES_SHA256="x")
        path = self.write("".join("%s=%s\n" % item for item in lines.items()))
        done = self.run_fetch("--pin", path)
        self.assertEqual(done.returncode, 2)
        self.assertIn("SOURCES_SHA256 is not a SHA-256", done.stderr)

    def test_a_name_that_climbs_out_of_the_download_directory_is_refused(self):
        lines = {key: "" for key in FETCH.KEYS}
        lines.update(TAG="t", TARBALL="../tc.tar.gz", SOURCES="s.tar",
                     TARBALL_SIZE="1", SOURCES_SIZE="1", TARBALL_SHA256="0" * 64, SOURCES_SHA256="0" * 64)
        path = self.write("".join("%s=%s\n" % item for item in lines.items()))
        self.assertEqual(self.run_fetch("--pin", path).returncode, 2)


class Unpack(unittest.TestCase):
    """A pinned release whose files are in the download directory."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.dl = os.path.join(self.dir, "dl")
        self.out = os.path.join(self.dir, "out")
        os.makedirs(self.dl)
        self.tc = self.tarball("tc.tar.gz", {"x-toolchain/bin/gcc": b"gcc\n"})
        self.sources = os.path.join(self.dl, "sources.tar")
        with open(self.sources, "wb") as handle:
            handle.write(b"sources\n")
        self.pin = self.write_pin()

    def tarball(self, name, files, relocate=True):
        path = os.path.join(self.dl, name)
        with tarfile.open(path, "w:gz") as archive:
            members = dict(files)
            if relocate:
                top = next(iter(files)).split("/")[0]
                members[top + "/relocate-tc.sh"] = b"#!/bin/sh\necho here > relocated\n"
            for member, data in members.items():
                info = tarfile.TarInfo(member)
                info.size = len(data)
                info.mode = 0o755
                archive.addfile(info, io.BytesIO(data))
        return path

    def write_pin(self, **changes):
        def digest(path):
            with open(path, "rb") as handle:
                data = handle.read()
            return str(len(data)), hashlib.sha256(data).hexdigest()
        tc_size, tc_sha = digest(self.tc)
        src_size, src_sha = digest(self.sources)
        lines = dict(TAG="toolchain-1", TARBALL="tc.tar.gz", TARBALL_SIZE=tc_size,
                     TARBALL_SHA256=tc_sha, SOURCES="sources.tar", SOURCES_SIZE=src_size,
                     SOURCES_SHA256=src_sha)
        lines.update(changes)
        path = os.path.join(self.dir, "PIN")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("# a pin\n" + "".join("%s=%s\n" % item for item in lines.items()))
        return path

    def run_fetch(self, *args):
        env = {k: v for k, v in os.environ.items() if k not in ("GH_TOKEN", "GITHUB_TOKEN")}
        return subprocess.run([sys.executable, "-I", SCRIPT, "--pin", self.pin, "--dl", self.dl,
                               "--out", self.out, *args], capture_output=True, text=True, env=env)

    def test_the_toolchain_is_unpacked_relocated_and_its_directory_printed(self):
        done = self.run_fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        top = os.path.join(self.out, "toolchain-1", "x-toolchain")
        self.assertEqual(done.stdout, top + "\n")
        self.assertTrue(os.path.isfile(os.path.join(top, "bin", "gcc")))
        self.assertTrue(os.path.isfile(os.path.join(top, "relocated")))

    def test_a_second_run_does_nothing_and_needs_no_tarball(self):
        self.assertEqual(self.run_fetch().returncode, 0)
        top = os.path.join(self.out, "toolchain-1", "x-toolchain")
        os.remove(os.path.join(top, "relocated"))
        os.remove(self.tc)
        done = self.run_fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertFalse(os.path.exists(os.path.join(top, "relocated")))

    def test_a_tarball_that_is_not_the_pinned_one_is_refused_and_nothing_unpacked(self):
        self.pin = self.write_pin(TARBALL_SHA256="1" * 64)
        done = self.run_fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("the pin says", done.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.out, "toolchain-1")))

    def test_another_pinned_tarball_is_unpacked_again(self):
        self.assertEqual(self.run_fetch().returncode, 0)
        os.remove(self.tc)
        self.tc = self.tarball("tc.tar.gz", {"x-toolchain/bin/gcc": b"another gcc\n"})
        self.pin = self.write_pin()
        done = self.run_fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        with open(os.path.join(done.stdout.strip(), "bin", "gcc"), "rb") as handle:
            self.assertEqual(handle.read(), b"another gcc\n")

    def test_a_tarball_without_one_toolchain_directory_is_refused(self):
        os.remove(self.tc)
        self.tc = self.tarball("tc.tar.gz", {"a/x": b"1", "b/y": b"2"}, relocate=False)
        self.pin = self.write_pin()
        done = self.run_fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("not one toolchain directory", done.stderr)

    def test_the_sources_are_checked_and_their_path_printed(self):
        done = self.run_fetch("--sources")
        self.assertEqual((done.returncode, done.stdout), (0, self.sources + "\n"))
        self.pin = self.write_pin(SOURCES_SIZE="2")
        self.assertEqual(self.run_fetch("--sources").returncode, 1)


class Repository(unittest.TestCase):
    def test_the_variables_name_the_repository_first(self):
        old = {k: os.environ.get(k) for k in ("TOOLCHAIN_REPOSITORY", "GITHUB_REPOSITORY")}
        try:
            os.environ["GITHUB_REPOSITORY"] = "o/from-a-workflow"
            os.environ.pop("TOOLCHAIN_REPOSITORY", None)
            self.assertEqual(FETCH.repository(), "o/from-a-workflow")
            os.environ["TOOLCHAIN_REPOSITORY"] = "o/named"
            self.assertEqual(FETCH.repository(), "o/named")
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


class Redirect(unittest.TestCase):
    def test_the_token_does_not_follow_a_redirect_to_another_host(self):
        handler = FETCH._DropAuthorization()
        request = urllib.request.Request("https://api.github.com/repos/o/r/releases/assets/1",
                                         headers={"Authorization": "Bearer t"})
        new = handler.redirect_request(request, None, 302, "Found", {},
                                       "https://objects.githubusercontent.com/x")
        self.assertFalse(new.has_header("Authorization"))

    def test_the_token_follows_a_redirect_on_the_same_host(self):
        handler = FETCH._DropAuthorization()
        request = urllib.request.Request("https://api.github.com/a",
                                         headers={"Authorization": "Bearer t"})
        new = handler.redirect_request(request, None, 302, "Found", {}, "https://api.github.com/b")
        self.assertTrue(new.has_header("Authorization"))


if __name__ == "__main__":
    unittest.main()
