# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Which language a file is written in, and its token stream."""

import posixpath

from . import ctok, kconfig, make, yaml

# The architecture whose assembler reads the .S files of the tree.  Only
# assemblers that take '#' as a comment character get '#' comments.
DEFAULT_ARCH = 'mips'
_HASH_COMMENT_ARCHES = frozenset(('mips', 'x86'))

LANGUAGES = ('c', 'asm', 'dts', 'kconfig', 'make', 'yaml', 'defconfig')


def detect(path):
    """Return the language of *path* (by name), or None if it has none here."""
    name = posixpath.basename(path.replace('\\', '/'))
    if name.endswith(('.lds.S', '.lds')):
        return 'c'           # linker scripts: preprocessed, '#' is no comment
    if name.endswith(('.c', '.h')):
        return 'c'
    if name.endswith(('.S', '.s')):
        return 'asm'
    if name.endswith(('.dts', '.dtsi', '.dtso')):
        return 'dts'
    if name.endswith(('.yaml', '.yml')):
        return 'yaml'
    if name.startswith('Kconfig'):
        return 'kconfig'
    if (name in ('Makefile', 'makefile', 'GNUmakefile', 'Kbuild', 'Platform')
            or name.startswith(('Makefile.', 'Kbuild.'))
            or name.endswith(('.mk', '.mak'))):
        return 'make'
    if name.endswith(('.config', '_defconfig')) or name == 'defconfig':
        return 'defconfig'
    return None


def asm_hash_comments(path, arch=DEFAULT_ARCH):
    """Whether '#' starts a comment in the assembly file *path*.

    True when the tree is built for an architecture whose assembler says so
    and the file is not under another architecture's directory.
    """
    if arch not in _HASH_COMMENT_ARCHES:
        return False
    parts = path.replace('\\', '/').split('/')
    for k, part in enumerate(parts[:-1]):
        if part == 'arch' and k + 1 < len(parts) - 1:
            return parts[k + 1] == arch
    return True


def tokenize(text, language, path='', arch=DEFAULT_ARCH):
    """Return the canonical token list of *text* in *language*."""
    if language in ('c', 'dts'):
        return ctok.tokenize(text, language)
    if language == 'asm':
        return ctok.tokenize(text, 'asm', asm_hash_comments(path, arch))
    if language == 'kconfig':
        return kconfig.tokenize(text)
    if language == 'make':
        return make.tokenize(text)
    if language == 'yaml':
        return yaml.tokenize(text)
    if language == 'defconfig':
        return kconfig.fragment_tokens(text)
    raise ValueError('unknown language %r' % (language,))


def decode(data):
    """Bytes to text, keeping every byte (undecodable ones as surrogates)."""
    return data.decode('utf-8', 'surrogateescape')


def encode(text):
    return text.encode('utf-8', 'surrogateescape')
