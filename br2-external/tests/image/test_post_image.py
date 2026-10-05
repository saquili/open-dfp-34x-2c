# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Tests of board/dfp-34x-2c3/post-image.sh.

The script runs as a program, in a scratch directory that holds a kernel image
and a tree to pack. Most tests give it a stand-in for mksquashfs that records its
arguments, so they need nothing but a POSIX shell; the tests of the image
itself run the real mksquashfs and are skipped without it.
"""

import hashlib
import os
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
BOARD = os.path.join(os.path.dirname(os.path.dirname(HERE)), "board", "dfp-34x-2c3")
SCRIPT = os.path.join(BOARD, "post-image.sh")
REAL_MKSQUASHFS = shutil.which("mksquashfs")
EPOCH = "1747000000"
SHELLS = [s for s in (shutil.which("sh"), shutil.which("dash")) if s]

STUB = """\
#!/bin/sh
# Stands in for mksquashfs: records its arguments, whether SOURCE_DATE_EPOCH
# reached it and the files it was given to pack, and writes a fixed payload.
printf '%s\\n' "$@" > "$STUB_ARGS"
printf '%s\\n' "${SOURCE_DATE_EPOCH-unset}" > "$STUB_ARGS.epoch"
(cd "$1" && find . | sort) > "$STUB_ARGS.tree"
[ -z "$STUB_FAIL" ] || exit 3
cp "$STUB_PAYLOAD" "$2"
"""


class Scratch(unittest.TestCase):
    """A directory with images/, target/ and a stand-in for mksquashfs."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.images = os.path.join(self.root, "images")
        self.target = os.path.join(self.root, "target")
        os.makedirs(self.images)
        os.makedirs(os.path.join(self.target, "etc"))
        with open(os.path.join(self.target, "etc", "hostname"), "w") as handle:
            handle.write("board\n")
        self.kernel = b"\x27\x05\x19\x56 a kernel image\n" * 5
        with open(os.path.join(self.images, "uImage"), "wb") as handle:
            handle.write(self.kernel)
        self.payload = os.path.join(self.root, "payload")
        with open(self.payload, "wb") as handle:
            handle.write(b"hsqs a root filesystem\n")
        self.stub = os.path.join(self.root, "mksquashfs-stub")
        with open(self.stub, "w") as handle:
            handle.write(STUB)
        os.chmod(self.stub, 0o755)
        self.args_file = os.path.join(self.root, "args")

    def run_script(self, shell=None, mksquashfs=None, **override):
        env = {"PATH": os.environ["PATH"], "TARGET_DIR": self.target,
               "SOURCE_DATE_EPOCH": EPOCH, "MKSQUASHFS": mksquashfs or self.stub,
               "STUB_ARGS": self.args_file, "STUB_PAYLOAD": self.payload}
        env.update(override)
        env = {k: v for k, v in env.items() if v is not None}
        argv = ([shell] if shell else []) + [SCRIPT, self.images]
        return subprocess.run(argv, capture_output=True, text=True, env=env)

    def read(self, name, mode="rb"):
        with open(os.path.join(self.images, name), mode) as handle:
            return handle.read()

    def names(self):
        return sorted(os.listdir(self.images))


class WithAStandIn(Scratch):
    def test_it_leaves_the_two_images_and_their_digests(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.names(), ["SHA256SUMS", "k0.uImage", "r0.squashfs", "uImage"])

    def test_the_kernel_image_is_the_kernels_unchanged(self):
        self.run_script()
        self.assertEqual(self.read("k0.uImage"), self.kernel)
        self.assertEqual(self.read("uImage"), self.kernel)

    def test_the_root_filesystem_image_is_what_mksquashfs_wrote(self):
        self.run_script()
        self.assertEqual(self.read("r0.squashfs"), b"hsqs a root filesystem\n")

    def test_the_digests_are_those_sha256sum_reads(self):
        self.run_script()
        want = []
        for name in ("k0.uImage", "r0.squashfs"):
            want.append("%s  %s" % (hashlib.sha256(self.read(name)).hexdigest(), name))
        self.assertEqual(self.read("SHA256SUMS", "r").splitlines(), want)
        if shutil.which("sha256sum"):
            done = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=self.images,
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_mksquashfs_is_asked_for_the_properties_of_the_image(self):
        self.run_script()
        with open(self.args_file) as handle:
            args = handle.read().splitlines()
        self.assertEqual(args[0], self.target)
        self.assertEqual(args[1], os.path.join(self.images, "r0.squashfs.part"))
        rest = args[2:]
        for flag in ("-noappend", "-reproducible", "-nopad", "-no-xattrs", "-no-exports",
                     "-all-root"):
            self.assertIn(flag, rest)
        for flag, value in (("-comp", "xz"), ("-b", "64K"), ("-all-time", EPOCH),
                            ("-mkfs-time", EPOCH), ("-root-mode", "755")):
            self.assertEqual(rest[rest.index(flag) + 1], value, flag)

    def test_the_marker_of_the_target_directory_is_left_out(self):
        self.run_script()
        with open(self.args_file) as handle:
            args = handle.read().splitlines()
        self.assertEqual(args[args.index("-e") + 1], "THIS_IS_NOT_YOUR_ROOT_FILESYSTEM")

    def test_the_compression_is_one_the_kernel_reads(self):
        # docs/handbook/boot-contract.rst: xz (4).
        self.run_script()
        with open(self.args_file) as handle:
            args = handle.read().splitlines()
        self.assertEqual(args[args.index("-comp") + 1], "xz")

    def test_mksquashfs_does_not_see_the_variable_it_refuses_beside_a_time(self):
        self.run_script()
        with open(self.args_file + ".epoch") as handle:
            self.assertEqual(handle.read().strip(), "unset")

    def test_the_time_is_the_builds_and_not_the_clocks(self):
        self.run_script(SOURCE_DATE_EPOCH="1234567890")
        with open(self.args_file) as handle:
            args = handle.read().splitlines()
        self.assertEqual(args[args.index("-all-time") + 1], "1234567890")
        self.assertEqual(args[args.index("-mkfs-time") + 1], "1234567890")

    def test_a_second_run_gives_the_same_files(self):
        self.run_script()
        first = {n: self.read(n) for n in self.names()}
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual({n: self.read(n) for n in self.names()}, first)

    def test_it_runs_under_every_posix_shell_there_is(self):
        for shell in SHELLS:
            with self.subTest(shell=shell):
                done = self.run_script(shell=shell)
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_it_reports_the_sizes(self):
        done = self.run_script()
        self.assertIn("%d bytes in k0.uImage" % len(self.kernel), done.stdout)
        self.assertIn("%d bytes in r0.squashfs" % os.path.getsize(self.payload), done.stdout)


class Refusals(Scratch):
    def assertRefused(self, done, text):
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn(text, done.stderr)
        self.assertEqual([n for n in self.names() if n.endswith(".part")], [])

    def test_no_kernel_image(self):
        os.remove(os.path.join(self.images, "uImage"))
        self.assertRefused(self.run_script(), "no kernel image")
        self.assertNotIn("r0.squashfs", self.names())

    def test_no_build_time(self):
        done = self.run_script(SOURCE_DATE_EPOCH=None)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("SOURCE_DATE_EPOCH", done.stderr)
        self.assertEqual(self.names(), ["uImage"])

    def test_no_target_tree(self):
        done = self.run_script(TARGET_DIR=None)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("TARGET_DIR", done.stderr)

    def test_a_target_that_is_not_a_directory(self):
        self.assertRefused(self.run_script(TARGET_DIR=os.path.join(self.root, "none")),
                           "is not a directory")

    def test_no_mksquashfs(self):
        self.assertRefused(self.run_script(MKSQUASHFS=os.path.join(self.root, "none")),
                           "no mksquashfs")

    def test_no_host_directory_and_no_override(self):
        done = self.run_script(MKSQUASHFS=None)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("HOST_DIR", done.stderr)

    def test_a_missing_images_directory(self):
        done = subprocess.run([SCRIPT, os.path.join(self.root, "none")], capture_output=True,
                              text=True, env={"PATH": os.environ["PATH"], "TARGET_DIR": self.target,
                                              "SOURCE_DATE_EPOCH": EPOCH, "MKSQUASHFS": self.stub})
        self.assertEqual(done.returncode, 1)
        self.assertIn("is not a directory", done.stderr)

    def test_no_argument(self):
        done = subprocess.run([SCRIPT], capture_output=True, text=True,
                              env={"PATH": os.environ["PATH"]})
        self.assertEqual(done.returncode, 1)
        self.assertIn("usage", done.stderr)

    def test_a_failing_mksquashfs_leaves_no_half_image_and_no_digests(self):
        done = self.run_script(STUB_FAIL="1")
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(self.names(), ["k0.uImage", "uImage"])

    def test_a_failing_run_keeps_the_images_of_the_run_before(self):
        self.run_script()
        before = {n: self.read(n) for n in self.names()}
        self.run_script(STUB_FAIL="1")
        self.assertEqual({n: self.read(n) for n in self.names()}, before)


class BenchFiles(Scratch):
    """A directory of the files of one bench, and a scratch directory for the copy of the tree."""

    def setUp(self):
        super().setUp()
        self.bench = os.path.join(self.root, "bench")
        os.makedirs(os.path.join(self.bench, "root", ".ssh"))
        os.makedirs(os.path.join(self.bench, "etc"))
        with open(os.path.join(self.bench, "root", ".ssh", "authorized_keys"), "w") as handle:
            handle.write("ssh-ed25519 AAAA test@example\n")
        with open(os.path.join(self.bench, "etc", "board.conf"), "w") as handle:
            handle.write("DATA_ADDRESS=203.0.113.2/30\n")
        self.scratch = os.path.join(self.root, "tmp")
        os.makedirs(self.scratch)

    def packed(self):
        with open(self.args_file + ".tree") as handle:
            return handle.read().split()

    def run_with(self, **override):
        env = dict(OPEN_DFP_34X_2C_LOCAL_OVERLAY=self.bench, TMPDIR=self.scratch)
        env.update(override)
        return self.run_script(**env)


class WithAStandInAndALocalOverlay(BenchFiles):
    """OPEN_DFP_34X_2C_LOCAL_OVERLAY: the files of one bench, laid over a copy of the tree."""

    def test_the_files_of_the_bench_are_packed_with_the_target_tree(self):
        done = self.run_with()
        self.assertEqual(done.returncode, 0, done.stderr)
        tree = self.packed()
        for path in ("./etc/hostname", "./etc/board.conf", "./root/.ssh/authorized_keys"):
            self.assertIn(path, tree)

    def test_what_is_packed_is_a_copy_and_the_target_tree_is_not_touched(self):
        done = self.run_with()
        self.assertEqual(done.returncode, 0, done.stderr)
        with open(self.args_file) as handle:
            packed_from = handle.read().splitlines()[0]
        self.assertNotEqual(packed_from, self.target)
        self.assertEqual(sorted(os.listdir(self.target)), ["etc"])
        self.assertEqual(sorted(os.listdir(os.path.join(self.target, "etc"))), ["hostname"])

    def test_the_next_run_without_the_variable_packs_none_of_them(self):
        self.run_with()
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("./root/.ssh/authorized_keys", self.packed())
        self.assertNotIn("./etc/board.conf", self.packed())

    def test_the_scratch_copy_is_removed(self):
        self.run_with()
        self.assertEqual(os.listdir(self.scratch), [])

    def test_the_scratch_copy_is_removed_when_mksquashfs_fails(self):
        done = self.run_with(STUB_FAIL="1")
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(os.listdir(self.scratch), [])

    def test_a_bench_that_is_not_a_directory_is_refused(self):
        done = self.run_with(OPEN_DFP_34X_2C_LOCAL_OVERLAY=os.path.join(self.root, "none"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("is not a directory", done.stderr)
        self.assertEqual(self.names(), ["uImage"])

    def test_an_empty_variable_is_no_overlay(self):
        done = self.run_with(OPEN_DFP_34X_2C_LOCAL_OVERLAY="")
        self.assertEqual(done.returncode, 0, done.stderr)
        with open(self.args_file) as handle:
            self.assertEqual(handle.read().splitlines()[0], self.target)


def superblock(data):
    """The fields of a SquashFS 4 superblock that the tests read."""
    (magic, inodes, mkfs_time, block_size, fragments, compression, block_log, flags,
     ids, major, minor) = struct.unpack("<IIIIIHHHHHH", data[:32])
    return {"magic": magic, "inodes": inodes, "mkfs_time": mkfs_time, "block_size": block_size,
            "compression": compression, "flags": flags, "major": major, "minor": minor}


NO_XATTRS = 0x200
EXPORTABLE = 0x80


@unittest.skipUnless(REAL_MKSQUASHFS, "mksquashfs is not installed (apt-get install squashfs-tools)")
class WithTheRealTool(Scratch):
    def make_tree(self, path, names, mtime):
        # One file is older than the build's time: it must get the build's time too.
        names = list(names)
        for number in names:
            directory = os.path.join(path, "dir%d" % number)
            os.makedirs(os.path.join(directory, "sub"))
            with open(os.path.join(directory, "file%d" % number), "w") as handle:
                handle.write("content %d\n" % number * 200)
            os.chmod(os.path.join(directory, "file%d" % number), 0o644)
            os.symlink("file%d" % number, os.path.join(directory, "link%d" % number))
            with open(os.path.join(directory, "sub", "tool"), "w") as handle:
                handle.write("#!/bin/sh\n")
            os.chmod(os.path.join(directory, "sub", "tool"), 0o755)
            os.utime(os.path.join(directory, "file%d" % number), (mtime, mtime))
        os.utime(os.path.join(path, "dir%d" % min(names), "file%d" % min(names)),
                 (1000000000, 1000000000))
        os.chmod(path, 0o755)

    def image_of(self, names, mtime):
        shutil.rmtree(self.target)
        os.makedirs(self.target)
        self.make_tree(self.target, names, mtime)
        done = self.run_script(mksquashfs=REAL_MKSQUASHFS)
        self.assertEqual(done.returncode, 0, done.stderr)
        return self.read("r0.squashfs")

    def test_the_image_has_the_properties_the_script_claims(self):
        data = self.image_of(range(1, 8), 1000000000)
        sb = superblock(data)
        self.assertEqual(sb["magic"], 0x73717368)  # "hsqs"
        self.assertEqual((sb["major"], sb["minor"]), (4, 0))
        self.assertEqual(sb["compression"], 4)  # xz, which the kernel reads
        self.assertEqual(sb["block_size"], 65536)
        self.assertEqual(sb["mkfs_time"], int(EPOCH))
        self.assertTrue(sb["flags"] & NO_XATTRS)
        self.assertFalse(sb["flags"] & EXPORTABLE)

    @unittest.skipUnless(shutil.which("unsquashfs"), "unsquashfs is not installed")
    def test_the_marker_of_the_target_directory_is_not_in_the_image(self):
        self.image_of(range(1, 3), 1000000000)
        with open(os.path.join(self.target, "THIS_IS_NOT_YOUR_ROOT_FILESYSTEM"), "w") as handle:
            handle.write("not a root filesystem\n")
        done = self.run_script(mksquashfs=REAL_MKSQUASHFS)
        self.assertEqual(done.returncode, 0, done.stderr)
        listing = subprocess.run(["unsquashfs", "-l", os.path.join(self.images, "r0.squashfs")],
                                 capture_output=True, text=True).stdout
        self.assertIn("squashfs-root/dir1/file1", listing)
        self.assertNotIn("THIS_IS_NOT_YOUR_ROOT_FILESYSTEM", listing)

    def test_the_image_is_not_padded_to_a_block(self):
        data = self.image_of(range(1, 8), 1000000000)
        self.assertNotEqual(len(data) % 4096, 0)

    def test_the_same_files_made_in_another_order_at_another_time_give_the_same_image(self):
        first = self.image_of(range(1, 8), 1000000000)
        second = self.image_of(reversed(range(1, 8)), 1600000000)
        self.assertEqual(hashlib.sha256(first).digest(), hashlib.sha256(second).digest())

    def test_another_file_gives_another_image(self):
        first = self.image_of(range(1, 8), 1000000000)
        second = self.image_of(range(1, 9), 1000000000)
        self.assertNotEqual(first, second)

    @unittest.skipUnless(shutil.which("unsquashfs"), "unsquashfs is not installed")
    def test_every_file_is_root_owned_and_has_the_builds_time(self):
        self.image_of(range(1, 4), 1000000000)
        done = subprocess.run(["unsquashfs", "-ll", "-no-progress",
                               os.path.join(self.images, "r0.squashfs")],
                              capture_output=True, text=True, env=dict(os.environ, TZ="UTC"))
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = [l for l in done.stdout.splitlines() if l.startswith(("d", "-", "l"))]
        self.assertGreater(len(lines), 10)
        for line in lines:
            self.assertIn(" root/root ", line)
            self.assertIn("2025-05-11 21:46", line)


@unittest.skipUnless(REAL_MKSQUASHFS and shutil.which("unsquashfs"),
                     "mksquashfs and unsquashfs are not installed (apt-get install squashfs-tools)")
class WithTheRealToolAndALocalOverlay(BenchFiles):
    def listing(self):
        done = subprocess.run(["unsquashfs", "-ll", os.path.join(self.images, "r0.squashfs")],
                              capture_output=True, text=True, env=dict(os.environ, TZ="UTC"))
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout

    def test_the_key_is_in_the_image_of_the_bench_and_not_in_the_next_one(self):
        plain = self.run_script(mksquashfs=REAL_MKSQUASHFS)
        self.assertEqual(plain.returncode, 0, plain.stderr)
        before = self.read("r0.squashfs")
        self.assertNotIn("authorized_keys", self.listing())
        keyed = self.run_with(MKSQUASHFS=REAL_MKSQUASHFS)
        self.assertEqual(keyed.returncode, 0, keyed.stderr)
        listing = self.listing()
        self.assertRegex(listing, r"-rw-r--r-- root/root +\d+ .* squashfs-root/root/.ssh/authorized_keys")
        self.assertIn("squashfs-root/etc/board.conf", listing)
        self.assertNotEqual(self.read("r0.squashfs"), before)
        again = self.run_script(mksquashfs=REAL_MKSQUASHFS)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(self.read("r0.squashfs"), before)

    def test_the_mode_of_a_file_of_the_bench_is_kept(self):
        key = os.path.join(self.bench, "root", ".ssh", "authorized_keys")
        os.chmod(key, 0o600)
        os.chmod(os.path.join(self.bench, "root", ".ssh"), 0o700)
        done = self.run_with(MKSQUASHFS=REAL_MKSQUASHFS)
        self.assertEqual(done.returncode, 0, done.stderr)
        listing = self.listing()
        self.assertRegex(listing, r"-rw------- root/root +\d+ .* squashfs-root/root/.ssh/authorized_keys")
        self.assertRegex(listing, r"drwx------ root/root +\d+ .* squashfs-root/root/.ssh\n")


class TheScriptItself(unittest.TestCase):
    def test_it_is_executable_and_names_its_shell(self):
        self.assertTrue(os.stat(SCRIPT).st_mode & stat.S_IXUSR)
        with open(SCRIPT) as handle:
            self.assertEqual(handle.readline().strip(), "#!/bin/sh")

    def test_it_parses_in_every_posix_shell_there_is(self):
        for shell in SHELLS:
            with self.subTest(shell=shell):
                done = subprocess.run([shell, "-n", SCRIPT], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)


if __name__ == "__main__":
    unittest.main()
