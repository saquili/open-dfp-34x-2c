.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========
tools/equiv
===========

The equivalence tools: they prove that the kernel code did not change while
its prose was re-authored and its identifiers were renamed.

Two kinds of proof are available, and they answer different questions:

* **Token identity.** Every source file is turned into a canonical token
  stream -- comments removed, the amount of whitespace ignored, identifiers
  and literals kept byte-exact. Two trees whose streams are equal file by
  file differ only in comments, whitespace and help or description prose.
  This is the proof for a rewrite of comments: it does not care that the
  lines moved.
* **Object identity.** Both trees are built reproducibly and the SHA-256 of
  every object is compared. This is the proof for renames that vanish in
  the preprocessor, such as register macros. It needs a build that is
  reproducible first: when the same tree built twice differs, a comparison
  of two different trees proves nothing.

Everything here is Python 3 (standard library only) and POSIX shell. The
tools that build run in a toolchain container; see `Building`_.

The tools
=========

Every tool prints its options with ``--help``.

``c-tokens FILE...``
    Print the canonical token stream, one token per line as
    ``KIND<TAB>TEXT`` (``--lines`` adds the line number). The language
    comes from the file name, or from ``--lang``.

``kconfig-strip FILE...``, ``yaml-strip FILE...``
    Print a Kconfig file without comments and help text, or a binding schema
    without comments, ``description`` and ``title``, for reading or for a
    plain ``diff``.

``compare-files A B``
    Classify the difference between two files (see `Classes`_); list the
    changed literals, or the first differing token runs of a code change.

``compare-trees A B``
    Classify every file that differs between two directories. With
    ``--git REPO``, A and B are revisions or tree-ishes of the git
    repository REPO (``B4``, ``HEAD:linux``); nothing is checked out, so this
    works on a case-insensitive file system too. ``--json`` gives a
    machine-readable report, ``--brief`` one line per file.

``obj-manifest OUT``
    Print the SHA-256 manifest of the kernel build directory OUT in
    ``sha256sum`` format: ``.config``, ``vmlinux``, every ``*.o`` and ``*.ko``
    (host programs included), every ``*.dtb`` and ``*.dtbo``, and the regular
    files in ``arch/<arch>/boot/``. ``--target-only`` leaves out the objects
    of host programs.

``compare-objs A B``
    Compare two manifests (files, or build directories to record them from)
    and list what differs.

``kbuild-repro [OPTION...] TREE OUT [TARGET...]``
    Build the kernel tree TREE into OUT with everything pinned that would
    otherwise vary between two builds (see `Reproducible builds`_).

``in-toolchain --image IMAGE [--volume NAME] [--mount DIR]... -- COMMAND``
    Run COMMAND in the toolchain container, with a case-sensitive work volume
    at ``/work``.

``repro-check --image IMAGE --git REPO [OPTION...] -- [KBUILD-REPRO-ARG...]``
    Build one tree twice under conditions that differ and compare the two
    manifests: the proof that object comparisons of that tree mean
    something.

Classes
=======

``identical``
    The bytes are equal.
``comment``
    The token streams are equal: comments, whitespace or prose differ.
``literal``
    Only string literals differ. Each maximal run of adjacent literals
    counts as one, as the compiler joins them, and every changed run is
    listed; a run whose value stayed the same is marked. This is a class for
    review, not an equivalence: a literal can still change behaviour -- a
    ``compatible`` string, a buffer sized by ``sizeof``.
``code``
    Anything else, including a change of a file's mode or a symlink target.
``added``, ``removed``
    Present on one side only.
``other``
    The bytes differ and the file type has no token model (prose, scripts,
    binaries).
``error``
    The file cannot be tokenised, for example an unterminated comment.

The comparing tools exit with **0** when nothing differs beyond comments,
whitespace and prose, **3** when string literals differ and nothing else,
**1** for any other difference and **2** on an error.

The token models
================

The rule for every model: where the tool cannot follow the real reader of the
language, it keeps text together rather than apart, so that an error reports
a difference that is not there and never hides one that is.

C (``.c``, ``.h``, linker scripts ``.lds.S``)
    Comments are removed after line splicing, as the preprocessor removes
    them, and never inside literals. Punctuators are matched longest-first.
    Every preprocessor directive ends in an end-of-line token, and a macro
    name records whether ``(`` follows it without a blank. String literals
    are the literal class, except where their text is code: ``asm``
    templates, ``__attribute__`` and ``_Pragma`` arguments, file names of
    ``#include``, strings of other directives. The message of ``#error`` and
    ``#warning`` is one literal, spelt as the preprocessor spells it.
Assembly (``.S``)
    Preprocessed like C. For the architectures whose assembler takes ``#`` as
    a comment character (MIPS, x86), and for files outside another
    architecture's directory, ``#`` outside a directive starts a comment; a
    line that starts with ``#`` but names no directive is one everywhere.
    Every line that holds a statement ends in an end-of-line token. All
    strings are code.
Device tree (``.dts``, ``.dtsi``, ``.dtso``)
    Preprocessed like C; names may contain ``#``. String values are the
    literal class.
Kconfig
    Read line by line: every statement ends with its line. ``help`` text is
    prose and ends where the kconfig lexer ends it. The first string after
    ``bool``, ``tristate``, ``prompt``, ``menu`` and the other prompt keywords
    is the literal class; ``default`` values and ``source`` paths are code.
Configuration fragments (``*.config``, ``*_defconfig``)
    ``# CONFIG_X is not set`` is code; other ``#`` lines are comments.
Makefiles
    Every logical line becomes its words. Continuation lines are joined as
    make joins them. A ``#`` is a comment unless escaped or inside a
    reference. Recipe lines keep their quoted text; ``define`` bodies are
    kept verbatim. Not detected: a change of only the amount of whitespace
    inside function arguments or at the end of a value.
YAML binding schemas
    Line oriented; indentation is kept, as it is syntax. ``description`` and
    ``title`` entries are dropped with their values, keeping the ``-`` of a
    sequence entry they open, and comments are dropped outside block
    scalars. Other block scalars, such as ``examples``, are kept verbatim.
    All strings are code.

Reproducible builds
===================

``kbuild-repro`` builds out of tree (``make O=OUT``) and pins:

* ``KBUILD_BUILD_TIMESTAMP`` (``1970-01-01 00:00:00 UTC``) and
  ``SOURCE_DATE_EPOCH`` (the same instant), ``KBUILD_BUILD_USER`` and
  ``KBUILD_BUILD_HOST`` (``build``), ``KBUILD_BUILD_VERSION`` (``1``);
* ``LOCALVERSION`` set and empty, so that no ``+`` is appended;
* the build paths: ``-ffile-prefix-map`` and ``-fdebug-prefix-map`` from TREE
  and from OUT to ``.``, for every compiler the build runs -- ``KCFLAGS`` for
  kernel C; ``KCPPFLAGS`` for everything preprocessed; ``HOSTCFLAGS`` and
  ``HOSTCXXFLAGS`` for host programs; ``USERCFLAGS`` for user programs;
* ``LC_ALL=C``, ``TZ=UTC``, ``umask 022``, and nothing else of the caller's
  environment: make runs under ``env -i``.

The MIPS vDSO is why ``KCFLAGS`` alone is not enough: its makefile replaces
``KBUILD_CFLAGS``, losing ``KCFLAGS``, and always compiles with debug
information. Its objects then carry the absolute source and build paths, and
the build ID of the vDSO, computed over those, reaches ``vmlinux``.
``KBUILD_CPPFLAGS``, which carries ``KCPPFLAGS``, survives there.

The configuration is either a copy of a ``.config``, or a base target
(``tinyconfig``, a ``*_defconfig``) with fragments merged by
``scripts/kconfig/merge_config.sh`` and completed by ``olddefconfig``; every
line of every fragment must survive into the result. Refused, because they
make every build different: ``CONFIG_LOCALVERSION_AUTO``,
``CONFIG_MODULE_SIG_ALL`` and ``CONFIG_RANDSTRUCT``. A record of the build
-- tools, pinned values, configuration, and the version string the kernel
will print -- is written to ``OUT/kbuild-repro.info``.

Building
--------

The kernel tree holds paths that differ only in case, so it must be unpacked
and built on a case-sensitive file system; ``kbuild-repro`` refuses anything
else. ``in-toolchain`` provides one on any machine: it runs the toolchain
image with a Docker volume at ``/work``, which lives on the file system of
the container host, and mounts this repository and every ``--mount``
directory read-only at their own paths. A tree is streamed in, never copied
from a case-insensitive checkout::

    git -C REPO archive HEAD |
        tools/equiv/in-toolchain --image IMAGE -- sh -c \
            'mkdir -p /work/tree && tar -x -C /work/tree'
    tools/equiv/in-toolchain --image IMAGE --mount FRAGDIR -- \
        "$PWD/tools/equiv/kbuild-repro" --defconfig tinyconfig \
            --fragment FRAGDIR/board.config /work/tree /work/out all

Checking that a tree builds reproducibly
----------------------------------------

``repro-check`` does the whole check::

    tools/equiv/repro-check --image IMAGE --git REPO --rev REV \
        --mount FRAGDIR --target all --save RESULTS -- \
        --defconfig tinyconfig --fragment FRAGDIR/board.config

It unpacks the tree twice and builds it twice. The second build differs in
the source and build paths, in the file times of the source, in the number of
parallel jobs, in the time of day, and in its caller: ``umask 077`` and a
foreign environment -- time zone, locale, ``KBUILD_BUILD_*``,
``SOURCE_DATE_EPOCH``, and ``KAFLAGS=-g``, which would change every assembly
object if it reached the build. The exit status is 0 only if the two
manifests are identical. ``--b-arg`` adds arguments to the second build only:
with ``--b-arg --build-version --b-arg 2`` the check must fail and name the
objects that carry the build number, which shows that it can fail.

Self-tests
==========

``tests/run`` runs them (``-v`` for one line per test)::

    tools/equiv/tests/run

They check, for C, assembly, device tree, Kconfig, makefiles and YAML, that
a comment-only edit and a whitespace edit compare identical, that an
identifier rename is a code difference, and that a string change is reported
as literal-only in C, device tree and Kconfig -- and as code in the languages
whose strings are code. Further tests pin down the lexers against what the
preprocessor does, the tree walk over directories and git revisions, the
manifests, and every tool's exit status and argument checks. They need
Python 3; the tests of the git mode also need git and are skipped without
it. Run them inside the toolchain container as well to exercise
``kbuild-repro`` on a case-sensitive file system::

    tools/equiv/in-toolchain --image IMAGE -- \
        sh -c "cd /tmp && $PWD/tools/equiv/tests/run"

The reproducibility of a real tree is not a unit test: it is ``repro-check``,
run on that tree with the configuration it ships with.
