# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of buildroot/fetch: the pin, the hash gate, the patches and the stamp.

Each test makes a small release tarball, a pin for it and some patches in a
scratch directory, and runs the script as a program, so that what is tested is
what a user runs: the exit status, what is printed and what is left on disk.
There is no network; a "download" is a file: URL.
"""

import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
FETCH = os.path.join(os.path.dirname(HERE), "fetch")

SIGNER = "18C7DF2819C1733D822D599EA500D6EE9CB0E540"

HELLO = "one\ntwo\nthree\n"

PATCH_TWO_TO_UPPER = """\
--- a/hello.txt
+++ b/hello.txt
@@ -1,3 +1,3 @@
 one
-two
+TWO
 three
"""

# Applies only after the patch above: it expects the line that one writes.
PATCH_UPPER_TO_BANG = """\
--- a/hello.txt
+++ b/hello.txt
@@ -1,3 +1,3 @@
 one
-TWO
+Two!
 three
"""

# The context does not match the release.
PATCH_NO_MATCH = """\
--- a/hello.txt
+++ b/hello.txt
@@ -1,3 +1,3 @@
 one
-zzz
+yyy
 three
"""

PATCH_NEW_FILE = """\
--- /dev/null
+++ b/added/new.txt
@@ -0,0 +1 @@
+added by a patch
"""


def make_release(directory, files=None, version="1.0", members=None):
    """Write fake-<version>.tar.xz holding the files under fake-<version>/."""
    top = "fake-" + version
    path = os.path.join(directory, top + ".tar.xz")
    files = {"hello.txt": HELLO, "sub/code.c": "int main(void) { return 0; }\n"} \
        if files is None else files
    os.makedirs(directory, exist_ok=True)
    with tarfile.open(path, "w:xz") as archive:
        info = tarfile.TarInfo(top)
        info.type = tarfile.DIRTYPE
        info.mode = 0o755
        archive.addfile(info)
        for relative, text in files.items():
            data = text.encode()
            info = tarfile.TarInfo("%s/%s" % (top, relative))
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 1_000_000_000
            archive.addfile(info, io.BytesIO(data))
        for name, kind, linkname in members or ():
            info = tarfile.TarInfo(name)
            info.type = kind
            info.linkname = linkname
            info.mode = 0o644
            archive.addfile(info)
    return path


def sha256(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def write_pin(path, tarball, url=None, size=None, digest=None, extra=""):
    name = os.path.basename(tarball)
    assert name.endswith(".tar.xz")
    top = name[:-len(".tar.xz")]
    package, _, version = top.partition("-")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# a pin for the tests\n\n")
        handle.write("NAME=%s\nVERSION=%s\n" % (package, version))
        handle.write("URL=%s\n" % (url or "file://" + tarball))
        handle.write("SIZE=%d\n" % (os.path.getsize(tarball) if size is None else size))
        handle.write("SHA256=%s\n" % (digest or sha256(tarball)))
        handle.write("SIGNER=%s\n" % SIGNER)
        handle.write(extra)


class FetchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fetch-test-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.server = os.path.join(self.tmp, "server")   # where the URL points
        self.dl = os.path.join(self.tmp, "dl")
        self.build = os.path.join(self.tmp, "work", "build")
        self.patches = os.path.join(self.tmp, "patches")
        self.pin = os.path.join(self.tmp, "VERSION")
        os.makedirs(self.patches)
        self.tarball = make_release(self.server)
        write_pin(self.pin, self.tarball)
        self.tree = os.path.join(self.build, "fake-1.0")

    def patch(self, name, text):
        with open(os.path.join(self.patches, name), "w", encoding="utf-8") as handle:
            handle.write(text)

    def fetch(self, *extra, dl=None, patches=None):
        command = [sys.executable, FETCH, "--pin", self.pin, "--patches", patches or self.patches,
                   "--dl", dl or self.dl, "--build", self.build, *extra]
        env = dict(os.environ)
        env.pop("BUILDROOT_DL", None)
        return subprocess.run(command, capture_output=True, text=True, env=env)

    def read(self, relative):
        with open(os.path.join(self.tree, relative), encoding="utf-8") as handle:
            return handle.read()

    def stamp(self):
        with open(self.tree + ".stamp", encoding="utf-8") as handle:
            return handle.read()

    def put_in_dl(self):
        os.makedirs(self.dl, exist_ok=True)
        shutil.copy(self.tarball, self.dl)


class ExtractAndPatch(FetchTest):
    def test_extracts_a_tarball_from_the_download_directory(self):
        self.put_in_dl()
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), self.tree)
        self.assertEqual(self.read("hello.txt"), HELLO)
        self.assertEqual(self.read("sub/code.c"), "int main(void) { return 0; }\n")

    def test_applies_the_patches_in_name_order(self):
        self.put_in_dl()
        # Written in the wrong order on purpose: the second patch applies
        # only once the first has been applied.
        self.patch("0002-bang.patch", PATCH_UPPER_TO_BANG)
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER)
        self.patch("0003-new-file.patch", PATCH_NEW_FILE)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), "one\nTwo!\nthree\n")
        self.assertEqual(self.read("added/new.txt"), "added by a patch\n")
        self.assertIn("applied 0001-upper.patch", done.stderr)

    def test_ignores_files_that_are_not_patches(self):
        self.put_in_dl()
        self.patch("README", "this is not a patch\n")
        self.patch("0001-notes.txt", PATCH_NO_MATCH)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), HELLO)

    def test_no_patch_directory_is_fine(self):
        self.put_in_dl()
        done = self.fetch(patches=os.path.join(self.tmp, "no-such-directory"))
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_a_patch_that_does_not_apply_is_named_and_leaves_nothing(self):
        self.put_in_dl()
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER)
        self.patch("0002-does-not-fit.patch", PATCH_NO_MATCH)
        done = self.fetch()
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(done.returncode, 1)
        self.assertIn("0002-does-not-fit.patch", done.stderr)
        self.assertNotIn("0001-upper.patch does not apply", done.stderr)
        self.assertFalse(os.path.exists(self.tree))
        self.assertFalse(os.path.exists(self.tree + ".stamp"))
        # The scratch directory is gone too.
        self.assertEqual(os.listdir(self.build), [])

    def test_a_failed_run_leaves_the_earlier_tree_and_its_stamp_as_they_were(self):
        self.put_in_dl()
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER)
        self.assertEqual(self.fetch().returncode, 0)
        stamp = self.stamp()
        self.patch("0001-upper.patch", PATCH_NO_MATCH)
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("0001-upper.patch", done.stderr)
        # The stamp still says what the tree is, so the next run, which finds
        # the patches different from it, does the work again.
        self.assertEqual(self.read("hello.txt"), "one\nTWO\nthree\n")
        self.assertEqual(self.stamp(), stamp)
        self.assertEqual(os.listdir(self.build).count("fake-1.0"), 1)
        self.assertEqual([n for n in os.listdir(self.build) if n.startswith(".")], [])
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER.replace("+TWO", "+Two"))
        self.assertEqual(self.fetch().returncode, 0)
        self.assertEqual(self.read("hello.txt"), "one\nTwo\nthree\n")


class HashGate(FetchTest):
    def test_a_wrong_hash_is_refused_and_nothing_is_extracted(self):
        self.put_in_dl()
        write_pin(self.pin, self.tarball, digest="0" * 64)
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("SHA-256", done.stderr)
        self.assertIn("refusing", done.stderr)
        self.assertFalse(os.path.exists(self.build))

    def test_a_wrong_size_is_refused(self):
        self.put_in_dl()
        write_pin(self.pin, self.tarball, size=os.path.getsize(self.tarball) + 1)
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("bytes", done.stderr)
        self.assertFalse(os.path.exists(self.build))

    def test_a_tampered_tarball_in_the_cache_is_refused(self):
        self.put_in_dl()
        cached = os.path.join(self.dl, os.path.basename(self.tarball))
        with open(cached, "r+b") as handle:
            handle.seek(-4, os.SEEK_END)
            handle.write(b"\0\0\0\0")
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertFalse(os.path.exists(self.build))
        # The bad file is left for the user to look at, not replaced.
        self.assertTrue(os.path.exists(cached))

    def test_a_refused_patch_run_is_not_a_hash_failure(self):
        self.put_in_dl()
        self.patch("0001-upper.patch", PATCH_NO_MATCH)
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertNotIn("SHA-256", done.stderr)


class Download(FetchTest):
    def test_fetches_from_the_url_when_the_directory_has_no_tarball(self):
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("fetching file://", done.stderr)
        self.assertEqual(sha256(os.path.join(self.dl, "fake-1.0.tar.xz")), sha256(self.tarball))
        self.assertEqual(self.read("hello.txt"), HELLO)
        self.assertEqual([n for n in os.listdir(self.dl) if n.endswith(".part")], [])

    def test_a_download_of_other_bytes_is_refused_and_not_kept(self):
        other = make_release(os.path.join(self.tmp, "other"), files={"hello.txt": "other\n"})
        shutil.copy(other, os.path.join(self.server, "fake-1.0.tar.xz"))
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("refusing", done.stderr)
        self.assertEqual(os.listdir(self.dl), [])
        self.assertFalse(os.path.exists(self.build))

    def test_a_download_larger_than_the_pin_is_refused(self):
        write_pin(self.pin, self.tarball, size=os.path.getsize(self.tarball) - 1)
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("larger", done.stderr)
        self.assertEqual(os.listdir(self.dl), [])

    def test_an_unreachable_url_is_an_error_and_names_the_url(self):
        os.unlink(os.path.join(self.server, "fake-1.0.tar.xz"))
        done = self.fetch()
        self.assertEqual(done.returncode, 1)
        self.assertIn("cannot fetch file://", done.stderr)
        self.assertFalse(os.path.exists(self.build))

    def test_the_environment_names_the_download_directory(self):
        os.makedirs(self.dl)
        shutil.copy(self.tarball, self.dl)
        os.unlink(os.path.join(self.server, "fake-1.0.tar.xz"))  # only the cache has it
        command = [sys.executable, FETCH, "--pin", self.pin, "--patches", self.patches,
                   "--build", self.build]
        env = dict(os.environ, BUILDROOT_DL=self.dl)
        done = subprocess.run(command, capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), HELLO)


class Stamp(FetchTest):
    def first_run(self):
        self.put_in_dl()
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue(os.path.exists(self.tree + ".stamp"))

    def test_the_stamp_lists_the_pin_and_each_patch(self):
        self.first_run()
        stamp = self.stamp()
        self.assertIn("sha256 " + sha256(self.tarball), stamp)
        self.assertIn("patch 0001-upper.patch " + hashlib.sha256(PATCH_TWO_TO_UPPER.encode())
                      .hexdigest(), stamp)

    def test_a_second_run_changes_nothing_and_needs_no_tarball(self):
        self.first_run()
        marker = os.path.join(self.tree, "hello.txt")
        before = (os.stat(marker).st_mtime_ns, os.stat(self.tree).st_ino,
                  os.stat(self.tree + ".stamp").st_mtime_ns, self.stamp())
        time.sleep(0.05)
        # An empty directory and a URL that leads nowhere: the run must not
        # look at either.
        os.unlink(os.path.join(self.server, "fake-1.0.tar.xz"))
        done = self.fetch(dl=os.path.join(self.tmp, "empty-dl"))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("up to date", done.stderr)
        self.assertEqual(done.stdout.strip(), self.tree)
        after = (os.stat(marker).st_mtime_ns, os.stat(self.tree).st_ino,
                 os.stat(self.tree + ".stamp").st_mtime_ns, self.stamp())
        self.assertEqual(before, after)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "empty-dl")))

    def test_a_changed_patch_redoes_the_work(self):
        self.first_run()
        self.assertEqual(self.read("hello.txt"), "one\nTWO\nthree\n")
        old_stamp = self.stamp()
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER.replace("+TWO", "+Two"))
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("up to date", done.stderr)
        self.assertEqual(self.read("hello.txt"), "one\nTwo\nthree\n")
        self.assertNotEqual(self.stamp(), old_stamp)

    def test_an_added_patch_redoes_the_work(self):
        self.first_run()
        self.patch("0002-bang.patch", PATCH_UPPER_TO_BANG)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), "one\nTwo!\nthree\n")

    def test_a_removed_patch_redoes_the_work_from_a_pristine_tree(self):
        self.first_run()
        os.unlink(os.path.join(self.patches, "0001-upper.patch"))
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), HELLO)

    def test_a_changed_pin_redoes_the_work(self):
        self.first_run()
        respun = make_release(os.path.join(self.tmp, "respun"),
                              files={"hello.txt": "one\ntwo\nthree\n", "sub/code.c": "/* v2 */\n"})
        shutil.copy(respun, self.dl)                      # the same name, other bytes
        write_pin(self.pin, respun, url="file://" + self.tarball)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("sub/code.c"), "/* v2 */\n")
        self.assertIn("sha256 " + sha256(respun), self.stamp())

    def test_a_tree_removed_by_hand_is_made_again(self):
        self.first_run()
        shutil.rmtree(self.tree)
        done = self.fetch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.read("hello.txt"), "one\nTWO\nthree\n")


class Unpacking(FetchTest):
    def refuse(self, members):
        self.tarball = make_release(os.path.join(self.tmp, "bad"), members=members)
        write_pin(self.pin, self.tarball)
        done = self.fetch(dl=os.path.join(self.tmp, "bad"))
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("refusing", done.stderr)
        self.assertFalse(os.path.exists(self.tree))
        return done

    def test_a_member_outside_the_top_directory_is_refused(self):
        self.refuse([("elsewhere.txt", tarfile.REGTYPE, "")])
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "elsewhere.txt")))

    def test_a_member_that_climbs_out_is_refused(self):
        self.refuse([("fake-1.0/../escaped.txt", tarfile.REGTYPE, "")])
        self.assertFalse(os.path.exists(os.path.join(self.build, "escaped.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "escaped.txt")))

    def test_an_absolute_member_is_refused(self):
        self.refuse([("/tmp/fetch-test-absolute.txt", tarfile.REGTYPE, "")])
        self.assertFalse(os.path.exists("/tmp/fetch-test-absolute.txt"))

    def test_a_file_written_through_a_link_that_leaves_the_tree_is_refused(self):
        self.refuse([("fake-1.0/out", tarfile.SYMTYPE, "../.."),
                     ("fake-1.0/out/through.txt", tarfile.REGTYPE, "")])
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "through.txt")))

    def test_a_link_to_an_absolute_path_on_the_target_is_kept(self):
        # A release can ship links that mean something on the target only.
        self.tarball = make_release(os.path.join(self.tmp, "links"),
                                    members=[("fake-1.0/log", tarfile.SYMTYPE, "/var/log")])
        write_pin(self.pin, self.tarball)
        done = self.fetch(dl=os.path.join(self.tmp, "links"))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(os.readlink(os.path.join(self.tree, "log")), "/var/log")

    def test_a_file_that_is_not_an_xz_archive_is_refused(self):
        junk = os.path.join(self.tmp, "junk", "fake-1.0.tar.xz")
        os.makedirs(os.path.dirname(junk))
        with open(junk, "wb") as handle:
            handle.write(b"this is not an archive\n" * 10)
        write_pin(self.pin, junk)
        done = self.fetch(dl=os.path.dirname(junk))
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("cannot be unpacked", done.stderr)
        self.assertFalse(os.path.exists(self.tree))


class PinFile(FetchTest):
    def refuse_pin(self, text, needle):
        with open(self.pin, "w", encoding="utf-8") as handle:
            handle.write(text)
        done = self.fetch()
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn(needle, done.stderr)
        self.assertFalse(os.path.exists(self.build))

    def good_lines(self):
        with open(self.pin, encoding="utf-8") as handle:
            return [line for line in handle.read().splitlines() if "=" in line]

    def test_a_missing_key_is_an_error(self):
        lines = [line for line in self.good_lines() if not line.startswith("SHA256=")]
        self.refuse_pin("\n".join(lines) + "\n", "missing SHA256")

    def test_an_unknown_key_is_an_error(self):
        self.refuse_pin("\n".join(self.good_lines() + ["MIRROR=x"]) + "\n", "unknown key MIRROR")

    def test_a_key_set_twice_is_an_error(self):
        self.refuse_pin("\n".join(self.good_lines() + ["VERSION=2.0"]) + "\n", "set twice")

    def test_spaces_around_the_equals_sign_are_an_error(self):
        self.refuse_pin("\n".join(self.good_lines() + ["# x"]).replace("SIZE=", "SIZE = ") + "\n",
                        "no spaces")

    def test_a_short_hash_is_an_error(self):
        lines = [("SHA256=abc" if line.startswith("SHA256=") else line)
                 for line in self.good_lines()]
        self.refuse_pin("\n".join(lines) + "\n", "SHA256 is not 64")

    def test_a_url_that_is_not_https_is_an_error(self):
        lines = [("URL=http://example.org/fake-1.0.tar.xz" if line.startswith("URL=") else line)
                 for line in self.good_lines()]
        self.refuse_pin("\n".join(lines) + "\n", "URL must be")

    def test_a_missing_pin_file_is_an_error(self):
        os.unlink(self.pin)
        done = self.fetch()
        self.assertEqual(done.returncode, 2)
        self.assertIn("cannot read the pin", done.stderr)

    def test_the_real_pin_is_well_formed(self):
        real = os.path.join(os.path.dirname(HERE), "VERSION")
        command = [sys.executable, FETCH, "--pin", real, "--patches", self.patches,
                   "--dl", self.dl, "--build", self.build]
        # Nothing is fetched: the cache holds a file of the pinned name and
        # the wrong size, which the gate refuses before anything else.
        os.makedirs(self.dl)
        with open(os.path.join(self.dl, "buildroot-2025.02.18.tar.xz"), "wb") as handle:
            handle.write(b"not the release")
        done = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("the pin says 6043012", done.stderr)


class Tools(FetchTest):
    def test_a_missing_patch_program_is_reported(self):
        self.put_in_dl()
        self.patch("0001-upper.patch", PATCH_TWO_TO_UPPER)
        empty = os.path.join(self.tmp, "empty-path")
        os.makedirs(empty)
        command = [sys.executable, FETCH, "--pin", self.pin, "--patches", self.patches,
                   "--dl", self.dl, "--build", self.build]
        done = subprocess.run(command, capture_output=True, text=True,
                              env=dict(os.environ, PATH=empty))
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("patch program is not installed", done.stderr)
        self.assertFalse(os.path.exists(self.tree))


if __name__ == "__main__":
    unittest.main()
