.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
Updating the system
===================

The stick's flash has two banks, each a kernel and a root filesystem, and both
hold this system. An update goes into the bank that does not run. The boot
loader is then told to start that bank once, and the new system keeps it when
it comes up; when it does not, the stick restarts into the bank it ran before.
An update needs neither the boot loader's prompt nor a console, and a power loss
at any point leaves a stick that starts.

A stick as shipped takes this system the first time through its shipped
firmware's web page (below), or through the bench, which writes the flash
from the boot loader's prompt (``flashing.rst``) and is also the way to a
recovery. ``install-guide.rst`` walks through the install, an update and the
ways back in order; this page says how each works.

The parts
=========

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Part
     - What it does
   * - ``update.bin``
     - The update file that ``make image`` leaves beside ``k0.uImage`` and
       ``r0.squashfs`` (``building.rst``): a header with the board, the version
       (the image's ``VERSION_ID``: the time of its commit in UTC, as
       ``2026.10.19-2130``), the kernel image's time and the two lengths, the
       kernel image, the root filesystem image, and the SHA-256 of the three.
       ``onu-utils/src/update.h`` gives the layout. A release's file carries
       the release's signature behind the root filesystem (ADR 0022); the
       checksum guards against damage.
   * - ``onu-update``
     - The stick's program for the banks (``onu-utils``): it checks an update
       file and its signature, writes it into the bank that does not run and
       reads it back, reads a bank out as an update file, says each bank's
       SHA-256 and signature, arms a start of a bank once, commits the running
       bank, and prints or changes the boot loader's environment.
   * - ``onu-trial``
     - Run last by the boot script: after a start of a bank on trial, it
       commits the bank once the system is up, and restarts into the committed
       bank when it is not.
   * - The panel
     - System, Software lists system A (bank 0) and system B (bank 1; see
       ``boot-contract.rst``, "The names of a bank"), uploads an update file,
       offers to start the new system, downloads the running one,
       starts the other one when it holds a whole system of its own, goes back
       to the shipped firmware when the other bank holds that, and copies the
       whole flash. pond hands the upload on a pipe to onu-act, which runs
       ``onu-update`` on it (``pond/README.rst``).

Signatures
==========

The release workflow signs ``update.bin`` with the release key, whose public
half every image carries in ``/etc/onu-update.keys`` (ADR 0022). The stick
says what a file and each bank are signed with, and the panel shows it beside
each system, with the SHA-256 that the release's ``SHA256SUMS`` lists for
``update.bin``:

* ``release`` (``valid``): signed with a key of ``/etc/onu-update.keys``, whose
  name the panel shows under it.
* ``no signature`` (``none``): a build of the source, such as ``make image``
  leaves, or a bank that a release before signatures, or the bench from
  ``r0.squashfs``, wrote. It is written after a warning.
* ``unknown key`` (``unknown``): signed with a key the stick does not trust.
  It is written after a warning.
* ``does not match`` (``bad``): changed after it was signed. The browser does
  not send it, and ``onu-update`` does not write it.

The panel reads a file before it sends it, and its confirmation shows the
file's version, build time, SHA-256 and signature. The boot script has every
bank read once after a start, at the lowest priority, about 3 s a bank; the
panel says "being read" until then. On a computer, with a checkout of this
repository::

    br2-external/board/dfp-34x-2c3/signupdate.py check update.bin \
        br2-external/board/dfp-34x-2c3/overlay/etc/onu-update.keys

says the same as the stick, from openssl and python3. ``web-install.tar``
carries the signature as well, inside its root filesystem, so that a bank the
shipped firmware's page writes says ``release`` too; that page itself checks
only its ``md5.txt``.

Which bank runs
===============

The boot loader hands the kernel ``root=31:5`` for bank 0 and ``root=31:7`` for
bank 1 (``boot-contract.rst``), so the bank that runs is the one whose root
filesystem the kernel mounted. After a start from RAM the root filesystem is the
image's own and no bank runs, whatever ``root=`` says: ``onu-update`` then
writes only a bank that is named, and ``onu-trial`` does nothing.

``onu-update status`` prints the state as ``KEY=VALUE`` lines:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Key
     - Meaning
   * - ``RUNNING``
     - The bank that runs, or -1 after a start from RAM.
   * - ``COMMITTED``
     - The bank the boot loader starts (``sw_commit``).
   * - ``NEXT``
     - The bank the next start takes: the one armed for a trial, else the
       committed one.
   * - ``TRIAL``
     - Present while the running bank runs on trial, not committed yet.
   * - ``FAILED``
     - Present when the last trial of that bank did not come up: the loader
       took the trial and another bank runs.
   * - ``BANK<N>_STATE``
     - ``valid``, ``incomplete`` (a write that did not finish), ``shipped``
       (the shipped firmware: a kernel image, and a root filesystem where the
       shipped split puts it, ``fl_kernel<N+1>_sz`` of the loader's
       environment or 1328 KiB into the bank) or ``empty`` (no kernel image
       or no root filesystem image where they belong).
   * - ``BANK<N>_VERSION``
     - The running bank's ``VERSION_ID`` (its ``BUILD_ID``, the commit, for
       an image of before the versions); for the other bank the version that
       ``onu-update`` recorded when it wrote it.
   * - ``BANK<N>_TIME``, ``_KERNEL_SIZE``, ``_ROOTFS_SIZE``
     - The time in the bank's kernel image, and the sizes of its partitions.

Updating
========

From the panel: System, Software, "Upload new software", the file
``update.bin``, and root's password in the dialog that asks for it. The
dialog shows how much has gone; the stick then checks and reads back the
bank, some 45 seconds in all, and the card says which version it wrote where.
"Start system B" (or A), under Systems, restarts the stick into it, once;
with "Start it on trial right after the upload" turned on, the restart
follows the write by itself. ``install-guide.rst``
gives the steps with what the panel shows at each.

From a shell on the stick, with the file streamed in over ssh, or from
``/tmp``::

    ssh root@<stick> 'onu-update write -' < update.bin
    ssh root@<stick> 'onu-update try && reboot'

``onu-update write`` never touches the running bank. Before it writes anything
it checks the file's header: the board, and that the images fit the bank. A
file for another board, or one too big, leaves the bank as it was. It then marks
the bank ``sw_valid<N>=0``, writes the kernel and the root filesystem as they
arrive, erases what follows them in their partitions, checks the kernel image's
own CRC and the file's checksum, reads the two partitions back against the
checksum, and only then marks the bank ``sw_valid<N>=1`` with
``sw_version<N>``. A file whose checksum fails, or a write that does not finish,
leaves the bank marked invalid and its kernel's first sector erased.

``onu-update try`` arms ``sw_tryactive`` for the bank and refuses a bank that
holds no whole system. ``onu-update read`` writes the running bank as an update
file, which is the file that was written into it, byte for byte; the panel's
"Download the current software" is the same.

What happens at the start of a tried bank
=========================================

The boot loader sets ``sw_tryactive`` back to 2 and ``sw_active`` to the bank,
saves its environment, arms the watchdog and starts the bank. From then on,
every reset that is not a commit leads to the committed bank.

``onu-trial``, which the boot script starts first, waits for omcid to run as
one process across two looks five seconds apart (procd starts one that
crashed again after 2 s), for the status daemon to answer on the host link,
and for the PON to reach O5, operation, unless the receiver has no light or
the OLT keeps the ONU disabled, in O7: then the PON says nothing about the new
system. When all hold it runs ``onu-update commit``, which sets ``sw_commit``
to the running bank, removes the trial's mark and switches the loader's
multicast listener off (``mupgrade_en``). When they do not hold within 300
seconds it restarts the stick, and forces the restart when procd has not
carried it out in 30 s.
What it found is in ``/run/onu-trial`` and its log in ``/run/onu-trial.log``.

The environment as this system uses it
======================================

.. list-table::
   :header-rows: 1
   :widths: 24 20 56

   * - Variable
     - Written by
     - Meaning
   * - ``sw_commit``
     - ``onu-update commit``
     - The bank a plain start takes.
   * - ``sw_tryactive``
     - ``onu-update try``; the loader
     - 0 or 1: start that bank once; 2: no trial. The loader sets it to 2
       before it starts the tried bank.
   * - ``sw_active``
     - The loader
     - The bank it started last.
   * - ``sw_valid<N>``
     - ``onu-update write``
     - 0 while bank N is written or after a write that failed, 1 once it read
       back whole. ``try`` refuses a bank at 0.
   * - ``sw_version<N>``
     - ``onu-update write``
     - The version written into bank N. The shipped firmware wrote its own
       version names here.
   * - ``onu_trial``
     - ``onu-update try``, ``commit``
     - The bank armed for a trial, until a commit; with ``sw_tryactive`` back at
       2 and another bank running, that trial failed.
   * - ``mupgrade_en``
     - ``onu-update commit``
     - 0: the loader does not listen on the host link for an image sent by
       multicast, which it would write from anyone, by the shipped split, over
       this system's kernel (``boot-contract.rst``). Every way in ends in a
       commit, so a stick that runs this system has it at 0.

``onu-update`` saves the environment as the loader does: the whole new copy
into the copy that is not current, flagged 1, read back, and then the flag
byte of the old copy cleared to 0. Measured on the board, the loader's own save
writes 8192 bytes at the start of the other copy and then 1 byte at offset 4 of
the old one. A save cut short leaves one copy whose CRC checks out and whose
flag the loader takes.

What a failure costs
====================

.. list-table::
   :header-rows: 1
   :widths: 40 46 14

   * - Failure
     - What follows
     - How known
   * - A tried system that panics (a root filesystem without ``/sbin/init``)
     - ``panic=10`` restarts the stick after 10 s, and the loader starts the
       committed bank.
     - Measured
   * - A tried kernel that the loader cannot unpack (64 KiB of its data
       damaged, its CRCs made right)
     - The loader stops with ``LZMA: uncompress or overwrite error``; the
       watchdog it armed for the trial resets the chip, and the committed bank
       starts.
     - Measured
   * - A tried system that boots but does not come up
     - ``onu-trial`` restarts the stick after 300 s, into the committed bank.
     - Tested by function (``onu-utils/tests/test_onu_trial.sh``)
   * - A power loss during a write
     - The committed bank starts; the bank written reads ``incomplete`` and
       ``try`` refuses it until a new write.
     - Measured: the stick pulled from its cage during the second of a loop of
       writes of bank 1
   * - A damaged file
     - Refused when its checksum fails; the bank is left empty.
     - Measured through the panel's route
   * - A file for another board, or one too big
     - Refused before the bank is touched.
     - Tested by function

After every measured failure both copies of the environment read back with a
CRC that checks out, and the panel shows a failed trial as "last trial failed".

From the shipped firmware's web page
====================================

A stick as it ships runs the shipped firmware, and its web page takes an
update as a tar file that the shipped firmware installs into the bank it does
not run: ``web-install.tar``. The page takes a file smaller than its bank,
3932160 bytes, that holds ``md5.txt``. The shipped firmware checks the digest
of the package's ``fwu.sh`` against ``md5.txt``, runs it as root with the
bank to write, and restarts the stick. Nothing is signed or checked against
the model.

The package's ``fwu.sh`` (``br2-external/board/dfp-34x-2c3/fwu.sh``) writes
this system into that bank in the shipped firmware's split of it: ``k``, the
first 1328 KiB of this system's bank, into the shipped kernel partition, and
``r``, the rest, into the shipped root filesystem partition, so that the
kernel and the root filesystem land where this system's split has them. It
refuses a stick whose partitions are not this stick's, reads both partitions
back against the digests that ``mkweb.py`` wrote into it, and then sets
``sw_version``, ``sw_valid``, ``onu_trial`` and ``sw_tryactive`` for the
bank, as ``onu-update`` does. It exits 1 on purpose, and the page says the
update failed: the shipped firmware then leaves ``sw_commit`` on its own
bank, and the boot loader starts this system once. ``onu-trial`` commits it
when it is up; a system that does not come up within its 300 seconds is
restarted into the shipped firmware, and one that hangs in the kernel before
the boot script starts needs a power cycle (``boot-contract.rst``, "A trial
boot").

This way in has tests on the host, which run ``fwu.sh`` against stand-ins of
the stick's partitions and tools (``br2-external/tests/image/test_mkweb.py``),
and was measured on a stick on 2026-10-19 (M), with a later build of the
shipped firmware running from bank 1 and a package of this system built that
day (3338240 bytes):

* The page took the package and ran ``fwu.sh``, which logged ``into bank 0``,
  erased and wrote both partitions, and ``bank 0 is tried at the next
  start; the page says the update failed``. The stick restarted, the loader
  started bank 0 once, and this system came up from it, reached O5 nine
  seconds after the kernel started and committed its bank.
* The shipped firmware stayed in bank 1, in its own split. With ``sw_commit``
  set to 1 from this system (``onu-update env sw_commit=1``) and a restart, it
  started again, and the same login as before opened its page. A second
  upload through that page brought this system back, committed, with the
  shipped firmware still in bank 1.
* The stick had run this system before, so its file in ``config`` was there
  already, and it was the only file there: an earlier version of this system
  had kept its settings in place of the shipped firmware's files. The shipped
  firmware therefore started from its factory settings, both times. Two
  things were not part of this measurement: the first start of this system on
  a stick that never ran it, which takes the identity from the shipped
  firmware's files, and the shipped firmware's own settings kept across the
  round trip.

The same day both were measured on a stick whose whole flash, but for the
boot loader, held a copy of a stick as it was sold: the build of the shipped
firmware it was sold with in bank 0, an earlier one in bank 1, and the
shipped firmware's own files in ``config``, with no file of this system. The package was one of that
day, built without a bench's files but for the console's login (3338240
bytes):

* The shipped firmware's page took the package, and ``fwu.sh`` logged ``into
  bank 1``, the bank the shipped firmware did not run. This system came up from
  bank 1, reached O5 9.4 seconds after the kernel started and committed its
  bank.
* At this first start it took the serial number, the vendor ID and the PLOAM
  password from the shipped firmware's files, the values of
  ``lastgood_hs.xml`` and ``lastgood.xml``. Its own file in ``config`` held
  the panel's TLS key and the ssh server's host key and nothing else, and the
  shipped firmware's files kept the times the shipped firmware had last
  written them.
* With ``sw_commit`` set to 0 and a restart, the shipped firmware started
  from bank 0 with its own files, and its factory login opened its page.
* With the host's port at 2500BASE-X the link to the shipped firmware went
  down for about a minute at a time, and its page was reachable in between.
  With the host's port at 1000BASE-X without autonegotiation the link stayed
  up and the page answered throughout: set the host's port so for an
  install.

It was measured again on the same copy with a later package of that day,
whose first start also takes root's password, the host link's address and
its MAC address from the shipped firmware's files, the host's port at
1000BASE-X:

* On the shipped firmware's page its owner changed the administrator's
  password, which the shipped firmware wrote to its files, and the page took
  the package into bank 1.
* At the first start this system took that password for root's: the panel
  signed its owner in with it and asked for no new one. The host link and
  the switch's ports took the MAC address of the shipped firmware's files,
  the unit's own, and the identity was taken as before.
* The host link took the address of the shipped firmware's files as well, but
  answered nothing there: the OLT's service had put the host link in a bridge
  with a GEM port, where an address of the port itself takes no traffic. The
  panel was reached on the management VLAN. Since then the shipped firmware's
  address is the management VLAN's, VLAN 4094, and the host link itself has
  none.
* With ``sw_commit`` set to 0 and a restart, the shipped firmware started
  from bank 0, and the password its owner had set signed them in to its page.

It was measured again on 2026-10-20 with the files of the first release, as
the release workflow made them, through the shipped firmware's page of a stick
that ran it:

* This system's trial start reached O5 about 10 s after its kernel started,
  and committed its bank.
* The panel answered over HTTPS on VLAN 4094, at ``192.168.1.1``, with the
  certificate the stick signed itself, to a host with ``192.168.1.2/24`` on
  that VLAN, and signed in ``root`` with the password of the shipped
  firmware's administrator.
* "Go back to the shipped firmware" restarted the stick into the shipped
  firmware with its own settings, whose page answered at ``192.168.1.1``
  again, without a VLAN tag.

The new system finds the shipped firmware's files in the partition
``config``, mounts them, takes the identity from them and starts with its
settings' defaults, which it then keeps in a file of its own there
(``boot-contract.rst``). The shipped firmware stays in the other bank until
an update overwrites it. Going back to it for good is the panel's "Go back to
the shipped firmware" or ``onu-update return`` ("The first update", below);
for one start only, ``run b<bank>`` at the boot loader's prompt
(``recovery.rst``).

The first update
================

Until the first update, the bank this system does not run holds the shipped
firmware, in the shipped split: bank 1 after the bench, either bank after the
web page. ``onu-update`` reports that bank as ``shipped``, with the version the
shipped firmware recorded, and the panel names it so, warns under the upload
and asks before it writes over it. ``onu-update write`` on the command line
does not ask. The first update writes this system there, and the shipped
firmware is gone; the way back with it.

Keep a copy of the whole flash before the first update, at the latest right
after the install: over ssh, the way that was tried on a stick, or with "Copy
the whole flash" under System, Software, which has not been tried on a
stick yet (``backup.rst``). The copy holds the unit's registration identity
and this system's secrets: keep it private. ``backup.rst``, "Writing it back",
says how to write it back, which needs the serial console, a host port at
2500BASE-X and a RAM image built from the sources.

While the other bank holds the shipped firmware, the same page has "Go back to
the shipped firmware (system A)", or B, which asks for root's password, commits
that bank and restarts, so that the next start and every one after it are the
shipped firmware's, with its own settings (``POST /api/firmware/return``,
``onu-update return``). It refuses a bank whose kernel is not whole. Once the
first update has written over the shipped firmware the button is gone, and
``onu-update return`` refuses the bank.

A store from before 2026-10-11
==============================

Builds from before 2026-10-11 kept the store in the whole partition
``config``, in two slots of its own, where the shipped firmware's file system
had been; a stick that ran such a system keeps its settings, and the
registration identity it took from the shipped firmware, there. The first
start of a later system takes that store into the file
``/config/open-dfp-34x-2c.conf`` before it mounts the partition
(``onu-config.sh``, ``mount_config()``): it reads the lines of the current slot
into ``/run/onu-store.before``, erases the partition, mounts it
as an empty file system and writes the lines into the file. Between the erase
and the end of that write the lines are in RAM only, and a power loss there
loses the settings and the identity with them. The erase of the 240 KiB takes
seconds.

So the first start after such an update is attended:

1. Take a copy of the whole flash first (``backup.rst``), while the system of
   before still runs. It holds the store of before, slots and all.
2. Keep the stick powered until the system is up. Its log says ``the store
   of before goes into /config/open-dfp-34x-2c.conf`` as the conversion starts;
   when no line about ``/run/onu-store.before`` follows, it finished, and
   ``onu-config list`` shows the settings.
3. If the log says instead ``the store of before is in /run/onu-store.before
   only``, the write did not finish: copy that file off the stick before
   anything restarts it, since ``/run`` does not outlive a restart. Its lines
   go back with ``onu-config import``.

A stick that never ran a build from before 2026-10-11, one that took this
system through the shipped firmware's web page or through the bench since, has
no such store, and its first start writes nothing of the kind.

The shipped firmware's packages beside this system
==================================================

The packages of the shipped firmware were tried against the partition
``config`` that this system shares with it (ADR 0016; M: a stick of the
bench, 2026-10-11): the eight builds that the repository Anime4000/RTL960x
keeps for this stick, the one the stick shipped with among them.

* Each, written into the other bank in its own split and started from it,
  mounted the partition as it found it, with no format, took its two XML
  files as valid, and left this system's file as it was. After each, this
  system started again with its settings and the identity of the shipped
  files.
* A package other than the one a stick shipped with may write the shipped
  firmware's own files anew: on the bench one wrote the PLOAM password's
  format differently, so going back to another package than the stick's own
  can change how the stick introduces itself to an OLT. Going back, take the
  package the stick shipped with.

Measured on the board
=====================

.. list-table::
   :header-rows: 1
   :widths: 60 40

   * - Step
     - Time
   * - Writing and reading back a 3.6 MB update file streamed over ssh
     - 37 s
   * - The same, uploaded through the panel's route
     - 46 s
   * - From the end of the boot script to the commit of a tried bank, the PON
       in O5
     - 5 s
