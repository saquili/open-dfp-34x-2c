# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Object manifests of a build directory."""

import hashlib
import os
import shutil
import struct
import subprocess
import unittest

from equivtest import TempDirTest

from equivlib import manifest

MIPS, X86_64 = 8, 62


def elf(ei_class, ei_data, machine, body=b''):
    """The start of an ELF file: identification, type, machine."""
    order = '>' if ei_data == 2 else '<'
    return (b'\x7fELF' + bytes((ei_class, ei_data, 1)) + bytes(9)
            + struct.pack(order + 'HH', 1, machine) + body)


TARGET = elf(1, 2, MIPS)       # 32-bit, big-endian MIPS
HOST = elf(2, 1, X86_64)       # 64-bit, little-endian x86-64


class ManifestTest(TempDirTest):

    def build_dir(self):
        listed = {
            '.config': b'CONFIG_A=y\n',
            'vmlinux': TARGET + b'kernel',
            'vmlinux.o': TARGET + b'all',
            'init/main.o': TARGET + b'main',
            'drivers/x/x.ko': TARGET + b'module',
            'scripts/dtc/dtc.o': HOST + b'host',
            'arch/mips/boot/uImage.lzma': b'image',
            'arch/mips/boot/vmlinux.bin': b'raw',
            'arch/mips/boot/dts/acme/b.dtb': b'\xd0\x0d\xfe\xed',
            'arch/mips/boot/dts/acme/o.dtbo': b'\xd0\x0d\xfe\xed',
            'lib/empty.o': b'',
        }
        unlisted = {
            'System.map': b'map',
            'init/.main.o.cmd': b'cmd',
            'init/main.c.orig': b'',
            'arch/mips/boot/.uImage.cmd': b'cmd',
            'arch/mips/boot/compressed/x.bin': b'not a boot image',
            'modules.order': b'',
        }
        for rel, data in list(listed.items()) + list(unlisted.items()):
            self.write('out/' + rel, data)
        os.symlink('uImage.lzma',
                   os.path.join(self.tmp, 'out/arch/mips/boot/uImage'))
        return os.path.join(self.tmp, 'out'), listed

    def test_record_lists_what_it_should(self):
        out, listed = self.build_dir()
        entries = manifest.record(out)
        self.assertEqual([p for _, p in entries], sorted(listed))
        for sha, path in entries:
            self.assertEqual(sha, hashlib.sha256(listed[path]).hexdigest())

    def test_target_only_drops_host_objects(self):
        out, listed = self.build_dir()
        paths = [p for _, p in manifest.record(out, target_only=True)]
        self.assertNotIn('scripts/dtc/dtc.o', paths)
        self.assertIn('init/main.o', paths)
        self.assertIn('lib/empty.o', paths)     # not ELF: kept
        self.assertEqual(len(paths), len(listed) - 1)

    def test_target_only_needs_vmlinux(self):
        self.write('out/a.o', TARGET)
        with self.assertRaises(manifest.ManifestError):
            manifest.record(os.path.join(self.tmp, 'out'), target_only=True)
        self.assertEqual(len(manifest.record(os.path.join(self.tmp, 'out'))),
                         1)

    def test_not_a_directory(self):
        with self.assertRaises(manifest.ManifestError):
            manifest.record(os.path.join(self.tmp, 'missing'))

    def test_format_parse_and_diff(self):
        out, _ = self.build_dir()
        text = manifest.format_manifest(manifest.record(out))
        a = manifest.parse(text)
        self.assertEqual(manifest.load(out), a)
        b = dict(a)
        b['init/main.o'] = '0' * 64
        del b['vmlinux']
        b['new.o'] = '1' * 64
        self.assertEqual(manifest.diff(a, b),
                         (['vmlinux'], ['new.o'], ['init/main.o']))
        self.assertEqual(manifest.diff(a, a), ([], [], []))

    def test_parse_errors(self):
        with self.assertRaises(manifest.ManifestError):
            manifest.parse('abc  x.o\n')
        line = '%s  x.o\n' % ('a' * 64)
        with self.assertRaises(manifest.ManifestError):
            manifest.parse(line + line)

    def test_sha256sum_checks_a_manifest(self):
        out, _ = self.build_dir()
        path = os.path.join(self.tmp, 'm.sha256')
        with open(path, 'w') as f:
            f.write(manifest.format_manifest(manifest.record(out)))
        if shutil.which('sha256sum'):
            argv = ['sha256sum', '-c', '--quiet', path]
        elif shutil.which('shasum'):
            argv = ['shasum', '-a', '256', '-c', '--quiet', path]
        else:
            self.skipTest('no sha256sum or shasum')
        subprocess.run(argv, cwd=out, check=True)
        self.write('out/init/main.o', TARGET + b'changed')
        self.assertNotEqual(
            subprocess.run(argv, cwd=out, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL).returncode, 0)


if __name__ == '__main__':
    unittest.main()
