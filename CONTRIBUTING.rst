.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===============================
Contributing to open-dfp-34x-2c
===============================

Developer Certificate of Origin
===============================

Every commit must carry a ``Signed-off-by:`` line with the author's real name
and a reachable e-mail address, for example::

    Signed-off-by: Random Developer <random@example.org>

The sign-off certifies that you wrote the change, or otherwise have the right
to submit it under the repository's licence, in the terms of the Developer
Certificate of Origin 1.1 (https://developercertificate.org/). Use the identity
you are willing to publish. ``git commit -s`` adds the line for you.

Licensing
=========

Everything authored here is ``GPL-2.0-only``. Put the SPDX tag at the top of
every file you add, in that file's comment syntax, together with a copyright
line -- for a reStructuredText file, for instance::

    .. SPDX-License-Identifier: GPL-2.0-only
    .. SPDX-FileCopyrightText: 2026 Your Name <you@example.org>

``reuse lint`` must pass. Third-party code is never committed here; it is
fetched by hash at build time and keeps its own licence.

Provenance
==========

This project is built from original work, upstream open-source code, hardware
facts written in our own words with a stated way to verify them, and public
standards. Material of any other origin -- and the categories the policy names
-- must not enter. Read ``docs/policy/provenance.rst`` before you contribute
and follow it: the same rules bind code, comments, commit messages, generated
files and release assets, and the continuous-integration gates enforce them.

Commit messages
===============

Make one logical change per commit, with an imperative subject that states the
problem first and then the change.

* **Kernel commits** (under ``linux/``) use the kernel form
  ``subsystem: component: imperative summary`` -- for example
  ``net: dsa: realtek: rtl9601d: add PTP hardware clock`` or
  ``dt-bindings: net: add realtek,rtl9601d-eth``. The body explains the
  problem, then the change in hardware terms, cites standards by clause, and
  gives measurements as results on the named board, with counts.
* **Userspace and distribution commits** use their component prefix:
  ``omcid:``, ``package/omcid:``, ``board/<board>:``, ``configs/<board>:``,
  ``tools/bench:``, ``docs:``.
* **One component per commit**: a single commit touches only ``linux/``, only
  ``omcid/``, or only the distribution and tooling directories -- never a mix.
  This keeps each component liftable on its own and keeps the kernel history
  clean.

Trailers
========

* ``Signed-off-by:`` -- required, as above.
* ``Link:`` -- only to publicly reachable URLs.
* ``Fixes:`` -- only referencing commits of this repository.
* Tool assistance is credited with a ``Co-Authored-By:`` trailer. Do not use
  ``Co-developed-by:`` for a tool: the kernel defines it as authorship that
  must be followed by that co-author's own ``Signed-off-by:``.

A commit message must not contain comparative narrative about another
implementation, reverse-engineering vocabulary, third-party code names or
addresses, investigation history, private issue numbers, laboratory paths or
addresses, or unit identifiers. See ``docs/policy/provenance.rst``.

The kernel tree
===============

``linux/`` holds a Linux release exactly as upstream publishes it -- now Linux
6.18.48, from the kernel.org release tarball -- imported with
``git subtree add --squash``, and our kernel work on top of it as ordinary
kernel commits. Upstream files change only through such commits, never by
editing the import, and a kernel commit touches nothing outside ``linux/``.
That keeps the kernel history separable::

    git subtree split -P linux

prints a commit whose history is the imported release followed by exactly our
kernel commits, with the kernel at its root: a kernel branch to publish, to
rebase onto the upstream stable tag, or to turn into patches with
``git format-patch``.

The split starts at the commit the import was squashed from, which no branch
contains; the tag ``linux-6.18.48`` keeps it. Without the tag,
``git subtree split`` stops with "could not rev-parse split hash", so fetch and
push the tag along with the branches::

    git push origin main linux-6.18.48

Paths that differ only in case
------------------------------

The kernel holds 13 pairs of paths that differ only in case, such as
``net/netfilter/xt_DSCP.c`` and ``net/netfilter/xt_dscp.c``, and both files of
each pair are part of the kernel. A file system that ignores case -- the
default on macOS and on Windows -- keeps one file of each pair. Git keeps the
committed files right, but the working tree cannot hold them: ``git status``
always reports 13 modified paths (restoring one modifies its twin), and
whatever needs a clean working tree refuses to run, ``git rebase`` among them.

On such a file system, leave the pairs out of the working tree::

    git clone --no-checkout <repository> open-dfp-34x-2c
    cd open-dfp-34x-2c
    git show HEAD:tools/case-insensitive-checkout | sh
    git checkout main

``tools/case-insensitive-checkout`` sets up a sparse checkout without both
files of every pair; in an existing checkout, run it directly. The index
keeps all files, so commits, merges and ``git archive`` are unaffected, and
the board configuration builds none of the 26 files.

Work that changes those files -- importing a kernel release, below -- needs a
case-sensitive file system:

* on Linux, any native file system; on Windows, the file system of a WSL 2
  distribution;
* on macOS, a case-sensitive APFS volume -- one added with Disk Utility, or a
  disk image that grows as it fills::

      hdiutil create -type SPARSE -size 40g -fs 'Case-sensitive APFS' \
          -volname open-dfp-34x-2c open-dfp-34x-2c.sparseimage
      mkdir -p "$HOME/open-dfp-34x-2c-cs"
      hdiutil attach -nobrowse -mountpoint "$HOME/open-dfp-34x-2c-cs" \
          open-dfp-34x-2c.sparseimage

  and clone the repository into it;
* to build only, ``tools/equiv/in-toolchain``, which streams a tree into a
  case-sensitive volume of the toolchain container (see
  ``tools/equiv/README.rst``).

Updating to a newer release
---------------------------

A newer stable release comes in as the first one did: from its release
tarball, checked against the SHA-256 digest kernel.org signs in
``sha256sums.asc``, as one commit whose tree is the tarball's files and whose
parent is the previous release commit. On a case-sensitive file system, in
the unpacked tarball::

    export GIT_DIR=<repository>/.git GIT_WORK_TREE="$PWD"
    export GIT_INDEX_FILE="$(mktemp -u)"
    git add -A -f && tree=$(git write-tree) && rm -f "$GIT_INDEX_FILE"
    commit=$(git commit-tree -p 'linux-6.18.48^{commit}' \
        -m 'Linux 6.18.49' \
        -m 'The release tarball linux-6.18.49.tar.xz, SHA-256 <digest>.' \
        "$tree")
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

then, in the repository::

    git tag -a -m 'Linux 6.18.49, as imported into linux/' \
        linux-6.18.49 "$commit"
    git subtree merge --squash -P linux \
        -m 'linux: update to Linux 6.18.49' linux-6.18.49

As the new release commit descends from the previous one, the squash commit
lists only the releases in between, and the gates treat it as pristine
upstream, as they treat the import. A merge from the upstream stable
repository instead would list that repository's whole history in the squash
commit, because the release commits here are not upstream's own.
