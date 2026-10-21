.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================================
Installing, updating and going back
===================================

This page walks a stick through this system in order: the install on a stick
that still runs its shipped firmware, the first sign-in to the panel, an update
to a newer release, and the ways back, to the other bank of this system and to
the shipped firmware. Each step says what to do, what the stick shows and how
long it takes. Why it works so is on the reference pages it names:
``updating.rst`` for the banks, the trial start and the package of the shipped
firmware's page, ``backup.rst`` for the copy of the flash, and
``recovery.rst`` for the serial console.

The commands are those of a Linux router whose SFP port is ``eth1``, with the
stick in its cage; the README's "Reaching the panel" gives the same for
RouterOS. The panel's words are those of its English pages. A browser set to
Polish gets the Polish ones, and the panel's language button switches them.

What you need
=============

* The stick, a DFP-34X-2C2 or DFP-34X-2C3 that runs its shipped firmware, in
  the SFP cage of a router or a computer whose port you can set up. One image
  is for both, but only a 2C3 has run it: on a 2C2 it has not been tried yet.
* The address of the shipped firmware's page, ``192.168.1.1`` as the stick is
  sold, and the password of its administrator.
* From a release, on the repository's Releases page: ``web-install.tar``,
  ``update.bin`` and ``SHA256SUMS``.

No console is needed. The serial console is the way out when the stick
answers nothing at all (``recovery.rst``), and the bench of ``flashing.rst``
is another way in, for a stick on a bench.

Read "Before you install" in the README first. The fibre may stay out of the
stick for the whole of this page: without light the trial start does not wait
for the PON (``updating.rst``, "What happens at the start of a tried bank").
With the fibre in, the stick introduces itself to the OLT with the identity it
takes from the shipped firmware's files.

The host's view of the stick
----------------------------

**The sticks this system was tested on do not carry the host's identity page
as shipped**, and a stick as sold may behave otherwise in a Linux host. The
host reads the stick's identity page, A0h at ``0x50`` on the cage's two-wire
bus. It is a part of its own, apart from the laser driver's private EEPROM,
which the host never sees, and nothing this system installs changes it
(``board.rst``, "The SFP connector").

As shipped, byte 65 of the page (``0x41``, the low byte of the option word)
reads ``0x1a``: it declares RX_LOS implemented (bit 1,
``SFP_OPTIONS_LOS_NORMAL`` of ``linux/include/linux/sfp.h``) besides TX_FAULT
and TX_DISABLE. On the sticks tested it was written to ``0x18``, which
declares TX_FAULT and TX_DISABLE alone, and the check byte CC_EXT, byte 95
(``0x5f``), the low byte of the sum of bytes 64 to 94, went from ``0x3d`` to
``0x3b`` with it. Measured on a Linux host:

* With RX_LOS declared, the host's SFP driver keeps the link down while RX_LOS
  is high. After a warm restart of the host it stayed in the state
  ``wait_los``; a cold power cycle sometimes brought the link up anyway, so
  only a warm restart tells. With the change the link came up every time.
* With RX_LOS declared, the host also takes the link down whenever the fibre
  is dark. The host link is the stick's only way in, for the panel and ssh,
  so the stick is lost exactly when you need to look at it.
* RX_LOS itself, pin 8 of the SFP connector, is driven in hardware by the
  laser driver: it followed pulls of the fibre to the second. A host that
  wants a signal of a dark fibre can read that line as a GPIO of its own.

Changing the page is optional, and at your own risk. It was done with
``i2cset`` from the Linux host, on the two-wire bus of the stick's cage, after
reading the part number in bytes 40 to 55 (``DFP-34X-2C3`` on the sticks
tested) so as not to write another module: a wrong write to another module on
the same bus can damage that module. The two bytes are byte 65, from ``0x1a``
to ``0x18``, and byte 95, the low byte of the sum of bytes 64 to 94 after that
change.

The page also gives the lane's speed. It declares no Ethernet compliance code
(byte 6 is 0) and a nominal bit rate of 2500 MBd (byte 12, ``0x19``), so a
Linux host picks the mode from the bit rate. By the kernel's SFP code
(``sfp_parse_support()`` in ``drivers/net/phy/sfp-bus.c``) that is
2500BASE-X on Linux 5.12 to 6.3 and from 6.8 on, 1000BASE-X before 5.12, and
no mode it can use on 6.4 to 6.7. Only hosts with Linux 6.12 and 6.18 were
tried; the rest is read from the kernel's source. Of the lane's modes, this
system finds 2500BASE-X by itself, and 1000BASE-X with auto-negotiation;
1000BASE-X without auto-negotiation, and 2500BASE-X with it, it runs when the
panel's SFP page sets them, and each of the four linked with a host in the
same mode (2026-10-20). A fix that came after the first release finds
1000BASE-X without auto-negotiation by itself too (a test image, 2026-10-21;
step 2).

In short: on the sticks this system was tested on, the change of byte 65 is
what lets a Linux host bring the 2.5 Gb/s link up every time, and keep it up
while the fibre is dark, with RX_LOS left to the host to read as a line of its
own. The 2.5 Gb/s itself is the bit rate of the page as shipped, which was not
changed (both pages read back so, 2026-10-20).

Step 1: Check the files
=======================

In the directory that holds the files::

    sha256sum -c --ignore-missing SHA256SUMS

On macOS, ``shasum -a 256 -c --ignore-missing SHA256SUMS``. Each file you
have must say ``OK``::

    update.bin: OK
    web-install.tar: OK

A file that says ``FAILED`` is damaged or is not the release's: download it
again. Nothing else checks who made a file: the package and the update file
carry a checksum against damage, not a signature (ADR 0014).

Step 2: Reach the shipped firmware's page
=========================================

The shipped firmware's page answers on the host link itself, without a VLAN
tag. Give the router's port an address of the page's network::

    ip address add 192.168.1.2/24 dev eth1
    ip link set eth1 up

The shipped firmware tries the lane's modes after it starts: with the port at
2500BASE-X its link went down for about a minute at a time, and at 1000BASE-X
without auto-negotiation it stayed up (``updating.rst``, "From the shipped
firmware's web page"). If the router lets you set the port's mode, set it to
1000BASE-X without auto-negotiation for this step. This system finds
2500BASE-X, and 1000BASE-X with auto-negotiation, by itself; it did not find
1000BASE-X without auto-negotiation in a test of 2026-10-20. A fix that finds
it, 10.4 and 11.3 s after the port's change in a test of 2026-10-21, came
after the first release. So with the first release, once the page has taken
the package (step 3), set the port back to 2500BASE-X, or to 1000BASE-X with
auto-negotiation, before you reach the panel (step 4). A Linux host of 6.12
or later takes 2500BASE-X from the stick's page ("The host's view of the
stick"): there, wait out the drops.

Open ``192.168.1.1`` in a browser and sign in to the shipped firmware's page
with its administrator's account. If it does not answer, wait a minute and
try again: the link may be down.

Two of the shipped firmware's settings carry over to this system, which reads
them from the shipped firmware's files:

* **Its administrator's password becomes root's**, the one password of the
  console, ssh and the panel, at this system's first start. If it is still
  the default, ``admin``, set the one you want on the shipped page now: the
  default is not taken, and the panel would then ask for a new password at
  the first sign-in instead (ADR 0009).
* **Its LAN address becomes the panel's**, on VLAN 4094, at every start
  until the panel moves the stick to an address of its own. If you gave the
  page another address, the panel is at that one, and the router needs an
  address of its network in the steps below.

The install is not expected to remove anything the shipped firmware runs or
keeps: its bank stays as it was (step 3), and this system keeps its own
settings beside the shipped firmware's, which it does not write. It does write
over the other bank, and what that bank held is gone (step 6). These are test
builds, though, tried on few sticks: while its page is open, write down what it
shows and you would need again, the line's identity (the GPON serial number,
and the PLOAM password or LOID if your line uses one), the MAC address and any
setting you changed. Step 6 then keeps a copy of the whole flash.

Step 3: Install the package
===========================

On the shipped firmware's page that upgrades its firmware, the one that takes
a file, choose ``web-install.tar`` and start the upgrade. The menus that lead
there differ between builds of the shipped firmware. Give that page
``web-install.tar``, never ``update.bin``: it takes only a tar file smaller
than 3932160 bytes that holds ``md5.txt``, which the release's package is.

What follows comes by itself:

1. The page takes the file and runs the package's script, which writes this
   system over the bank the shipped firmware does not run, reads it back, and
   has the boot loader start that bank once, at the next start. The shipped
   firmware stays in its own bank, which still starts by default.
2. The page says that the update failed. That is expected: the script reports
   a failure on purpose, so that the shipped firmware does not make the new
   bank the one that starts for good (``updating.rst``, "From the shipped
   firmware's web page").
3. The stick restarts, and starts this system on trial. In the test of the
   first release this system reached O5 about 10 s after its kernel started,
   and kept its bank by itself.

The time the page takes to write has not been measured. Leave the stick in
its cage until it has restarted.

Step 4: Reach the panel
=======================

This system answers on VLAN 4094 of the host link, at the address the shipped
firmware's page had; the host link itself has no address of the stick's
(README, "Reaching the panel"). Move the router's address onto the VLAN::

    ip address del 192.168.1.2/24 dev eth1
    ip link add link eth1 name eth1.4094 type vlan id 4094
    ip address add 192.168.1.2/24 dev eth1.4094
    ip link set eth1.4094 up

Keep that network on the VLAN alone: the same network on both ``eth1`` and
``eth1.4094`` leaves the router two routes to it. Some half a minute after its
restart the stick answers::

    ping -c 3 192.168.1.1

Open ``https://192.168.1.1/``. The panel answers over HTTPS; its port 80 only
sends a browser on to HTTPS. The browser warns about the certificate, which
the stick signed itself (ADR 0013). Its SHA-256 fingerprint is printed on the
console at every start, and shown in the panel under System, tab Access,
card "HTTPS of the panel", as "Certificate fingerprint (SHA-256)". Accept the
certificate to go on, and compare the fingerprint once you are in. A
certificate that an authority you trust signed may take its place there
later ("Keys of ssh and of the panel", below).

Step 5: Sign in
===============

The page "Sign in" has the user ``root`` filled in. Type the password of the
shipped firmware's administrator into "Password" and press "Sign in".

* If that password was taken, the panel opens. ssh takes it as well:
  ``ssh root@192.168.1.1``.
* If it was the default, ``admin``, sign in with ``admin``. The panel shows
  "Set a new password" and allows nothing else until one is saved: fill in
  "Current password" (``admin``), "New password" and "Repeat the new
  password", 8 to 128 characters, and press "Save password". Until then ssh
  takes keys only.

Then check the install under System, tab Software. The card "Updates" lists
system A (bank 0) and system B (bank 1):

* the one that runs, with the release's version, marked "running now" and
  "starts by default";
* the other one, marked "firmware the stick shipped with".

While the running one also shows "on trial", and the card says "System B runs
on trial and is kept once the stick is fully up." (or system A), the trial has
not ended: reload the page a few seconds later. A session ends after
15 minutes without a request; sign in again then.

Step 6: Copy the whole flash
============================

Keep a copy of the whole flash at the latest right after the install, before
the first ``update.bin``: once the first update has written over the shipped
firmware, the copy is the only way back to it ("After the first update",
below).

The install has written this system over the bank the shipped firmware does
not run (step 3), so no copy taken now holds what that bank held before, which
on one stick as sold was an earlier build of the shipped firmware
(``updating.rst``, "From the shipped firmware's web page"). These pages give
no way to copy the flash before the install.

The way that was tried on a stick is ssh, from the router, as root with the
password of step 5 (``backup.rst``, "Taking the copy")::

    ssh root@192.168.1.1 'n=0; for c in 64 2 2 60 464 496 464 496; do
        dd if=/dev/mtd${n}ro bs=4096 count=$c 2>/dev/null; n=$((n + 1))
    done' >flash.bin

The copy is one file of 8388608 bytes. Check it against the stick: the same
loop on the stick, into ``sha256sum``, has to give the digest of the file::

    ssh root@192.168.1.1 'n=0; for c in 64 2 2 60 464 496 464 496; do
        dd if=/dev/mtd${n}ro bs=4096 count=$c 2>/dev/null; n=$((n + 1))
    done | sha256sum'
    sha256sum flash.bin

Keep the digest with the copy.

The panel has a button for the same copy, which has not been tried on a stick
yet: under System, tab Software, type root's password into "Root password" and
press "Copy the whole flash". It is made to say "Reading the flash, about a
minute…" and then "The copy of the flash is saved.", and to have the browser
save ``buchos-gpon-flash-<version>.bin`` of 8388608 bytes. The name begins
with the product name the panel shows, BuchOS unless another was set under
System, tab Settings, card "Device", "Product name". Check the size, and the
digest against the stick's as above, before you rely on such a copy.

The copy holds the unit's registration identity, its MAC address and this
system's secrets: keep it private, off shared storage and out of any report.

Writing the copy back is work for a bench, not for the panel: it needs the
serial console, a host port that links at 2500BASE-X and a RAM image built
from this repository's sources, which is not among a release's files ("After
the first update", below; ``backup.rst``, "Writing it back").

Keys of ssh and of the panel
============================

Under System, tab Access, the panel changes the keys of the stick's two
servers. Each change there needs root's password again: type it into "Root
password" at the top of the tab; it goes with each change and is not kept. A
wrong one is refused, and counts as a wrong sign-in. These changes have been
tried against the panel's mock stick and pond's tests, not yet on a stick.

To log in to ssh with a key, paste one line of your public key, such as the
content of ``~/.ssh/id_ed25519.pub``, into "Public key" under "Keys for ssh
login (root)" and press "Add the key": Ed25519, ECDSA (P-256, P-384, P-521)
or RSA of 2048 to 4096 bits, eight keys at most. The row shows its comment,
kind and SHA-256 fingerprint, as ``ssh-keygen -l -f key.pub`` prints it.
``ssh root@192.168.1.1`` then takes the key. Once a key works, "Take keys
only, no password" makes the ssh server refuse root's password, as a key's
owner may want; the panel and the console still take the password. The
switch waits for a key, and the last key cannot be removed while it is on.

"Make a new host key" gives the ssh server a new key: an ssh client that knew
the old one refuses to connect, as for a key it does not know, until the line
of the stick is taken out of its ``known_hosts`` (``ssh-keygen -R
192.168.1.1``); compare the fingerprint it shows then with the one on the tab.

Under "HTTPS of the panel", "Make a new key" makes new certificates of the
stick's own; "Your own certificate" takes a certificate that an authority your
browsers trust signed, with the CA certificates between it and the
authority's root, in PEM, and its key, an unencrypted ECDSA key of P-256 (the
panel's HTTPS takes no other kind). It should name the address the panel is
opened at among its alternative names, ``IP:192.168.1.1`` for instance; the
panel warns when it does not, when it ends within 30 days, and when it has
ended. "Back to the stick's own certificate" takes it away. After each of
these the browser warns once about a certificate it was not shown before,
unless it trusts the authority: reload the page, and compare the fingerprint
the browser shows with the one on the tab. The console prints the fingerprint
of the certificate served at every start.

The keys and the certificate are kept in the stick's store, so they hold
after a restart and an update, and a copy of the flash holds them
(``backup.rst``).

Updating to a newer release
===========================

An update goes into the bank that does not run and starts there on trial; the
running system stays as it is, and is the way back until the new one is kept
(``updating.rst``).

**The first update writes over the shipped firmware**, which the other bank
holds until then. Take the copy of step 6 first. Once the write has begun, an
upload that fails leaves that bank empty, so a failed first update costs the
shipped firmware as well.

1. Check ``update.bin`` of the new release against its ``SHA256SUMS``, as in
   step 1.
2. Under System, tab Software, type root's password into "Root password".
3. Leave "Start it on trial right after the upload" off, so that the start
   waits for you. Turned on, it has the stick start the new system as soon as
   it is written, after one more question.
4. Press "Upload new software". While the other system is the shipped
   firmware, the panel warns under the button and asks first: "System B holds
   the firmware the stick shipped with. An update writes over it, and then the
   only way back to it is a copy of the whole flash taken before. Write over
   it?" Answer OK only with the copy in hand.
5. Choose ``update.bin``. The panel shows "Sending… N %", then "Writing and
   checking, about a minute…", and then "Written into system B, version
   <version>." The running system is not touched.
6. Press "Start the new software", and confirm: "The stick restarts from
   system B, on trial. If it does not come up within 5 minutes, it goes back
   to system A by itself. Go on?" The panel says "The stick is restarting from
   system B. This page reconnects by itself."
7. Some half a minute later the panel answers again. A restart ends every
   session: sign in again. System B shows "running now" and "on trial" until
   the stick keeps it, and then "starts by default"; system A holds the
   version of before.

The letters are those of a stick that runs system A; on one that runs system
B they are the other way round.

The stick keeps the new system once omcid runs steadily, the panel answers and
the PON is in O5 or has no light. When that does not come within 300 seconds,
it restarts into the old system by itself, and the panel shows the new one as
"last trial failed" (``updating.rst``, "What happens at the start of a tried
bank").

From a shell, the same is one stream and one start (``updating.rst``,
"Updating")::

    ssh root@192.168.1.1 'onu-update write -' < update.bin
    ssh root@192.168.1.1 'onu-update try && reboot'

Going back
==========

To the other system of this project
-----------------------------------

After an update both banks hold this system: the one that runs, and the one of
before. To go back to the one of before:

1. Under System, tab Software, type root's password into "Root password".
2. Press "Start system A", with the letter of the other bank, and confirm:
   "The stick restarts from system A, on trial. If it does not come up within
   5 minutes, it goes back to system B by itself. Go on?"
3. The stick restarts into it on trial and keeps it once it is up, as after
   an update; it then shows "running now" and "starts by default". The newer
   system stays in the other bank, and "Start system B" brings it back the
   same way.

From a shell: ``ssh root@192.168.1.1 'onu-update try && reboot'``.

The button shows while the other bank holds a whole system of this project.
Both systems read the same settings, in the partition ``config``. An update
writes over the bank that does not run, so the version of before is gone after
the next update: "Download the current software" saves the running system
first, as an update file, ``buchos-gpon-<version>.bin`` (named after the
product name, as the copy of the flash is), which "Upload new software"
takes back like ``update.bin``.

To the shipped firmware
-----------------------

While the other bank holds the shipped firmware, that is before the first
update:

1. Under System, tab Software, type root's password into "Root password".
2. Press "Go back to the shipped firmware (system A)", with the letter of the
   bank that holds it, and confirm: "The stick restarts into the firmware it
   shipped with, in system A, and starts it from now on. [...] Go on?"
3. The panel says "The stick is restarting into the shipped firmware. Its page
   is where it was before this system was installed." From now on every start
   is the shipped firmware's, with its own settings; this system stays in the
   other bank.
4. Move the router's address back onto the port itself::

       ip link delete eth1.4094
       ip address add 192.168.1.2/24 dev eth1

   Open ``192.168.1.1``: the shipped firmware's page, which signs you in as
   before the install. Its link may go down for about a minute at a time
   while it tries the lane's modes, as in step 2.

In the test of the first release the stick restarted into the shipped
firmware with its own settings, and its page answered at ``192.168.1.1``
without a VLAN tag.

From a shell: ``ssh root@192.168.1.1 'onu-update return && reboot'``. The way
back checks the shipped firmware's kernel image first, and refuses a bank
whose image does not check out ("the kernel image of bank 0 does not check
out"); the stick then stays as it is.

Back to this system from the shipped firmware
---------------------------------------------

Install the package again, steps 2 to 5. The shipped firmware's page writes it
into the bank it does not run, the one this system was in, and this system
comes back with its settings, its keys and its password, which it keeps in a
file of its own in the partition ``config``, beside the shipped firmware's
files (``updating.rst``, "From the shipped firmware's web page"). It reads the
identity, the LAN address and the MAC address from the shipped firmware's
files at every start, and the administrator's password only at its very first
one, while its own file is still empty: a password changed on the shipped page
in between does not replace root's.

After the first update
----------------------

The first update wrote this system over the shipped firmware. The panel then
offers no way back to it, and ``onu-update return`` refuses with "bank 1
holds no shipped firmware" (or bank 0). The one way back is the copy of the
whole flash taken before it (step 6), written back from a system started from
RAM. Besides the copy, that needs:

* the serial console, to stop the boot loader at its prompt
  (``recovery.rst``);
* a host port that links at 2500BASE-X, the only mode the boot loader runs
  the lane in (``board.rst``, "The host side");
* a RAM image built from this repository's sources with ``make ram-image``,
  which is not among a release's files (``building.rst``, "Putting the images
  on the board"; ``recovery.rst``, "Start a bank, or RAM, from the prompt";
  ``flashing.rst``).

``backup.rst``, "Writing it back", gives the steps. A copy was written back so
once, on 2026-10-12, and the banks it put back were not started then. Without
such a copy, nothing in this repository brings the shipped firmware back.

When something fails
====================

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - What you see
     - What it means, and what to do
   * - ``FAILED`` for a file in step 1
     - The file is damaged or is not the release's. Download it again.
   * - A Linux host shows no link with the stick in its cage, or loses it
       whenever the fibre is dark
     - The stick's page declares RX_LOS, as shipped, and the host's SFP
       driver waits for it ("The host's view of the stick"). On a Linux host
       of 6.4 to 6.7 the page gives no mode the host can use.
   * - The shipped firmware's page answers, then not for about a minute, then
       again
     - The shipped firmware tries the lane's modes. Set the router's port to
       1000BASE-X without auto-negotiation (step 2), and wait.
   * - The shipped firmware's page refuses the file
     - It is not a release's ``web-install.tar``, or it is damaged (step 1).
   * - The shipped firmware's page says the update failed
     - Expected (step 3). Wait for the restart, then step 4.
   * - After the install the panel does not answer on VLAN 4094, and the
       shipped firmware's page answers again without a tag
     - The stick runs the shipped firmware: the package's script refused the
       stick, whose partitions are not those of this stick's shipped firmware,
       or this system did not come up and its trial gave way after at most
       300 seconds. The shipped firmware's bank was not written. Install once
       more; if it ends the same way, the console shows why
       (``recovery.rst``).
   * - Nothing answers, five minutes after the restart: neither the panel on
       VLAN 4094 nor the shipped firmware's page without a tag
     - Check the router's side first: the port's link and its mode (step 2),
       ``eth1.4094`` up with its address. Then take the stick out of the cage
       and put it back: the boot loader starts the bank that starts by
       default. If still nothing answers, go to the serial console:
       ``recovery.rst``, "Where the stick is".
   * - "Wrong user or password."
     - The user is ``root``. After the first start the password is the shipped
       firmware's administrator's, if it was not ``admin`` and had at most 128
       characters and no control character; otherwise it is ``admin``. A
       stick that ran this system before keeps the password it had then. A
       lost password is replaced over the console (``recovery.rst``, "The
       password is lost").
   * - "Too many attempts. Try again in 30 s."
     - Five wrong passwords from one address stop its sign-ins for 30
       seconds. Wait.
   * - "The stick cannot read its settings, so no password is known. [...]"
     - The store in the partition ``config`` cannot be read, and the panel
       refuses every sign-in (``recovery.rst``, "The password is lost").
   * - "Wrong password. Root's password is needed for this."
     - "Root password" holds another password than root's. It counts as a
       wrong sign-in.
   * - "Not written: [...]" after an upload
     - The stick refused the file or the write failed; the reason follows.
       The running system is untouched. A file refused at its header (another
       board, images too big) leaves the other bank as it was; one refused
       later, or an upload cut off, leaves it empty. Check ``update.bin``
       (step 1) and upload it again.
   * - "The last trial of system B did not come up, so the stick went back to
       the other system."
     - The new system did not come up within 300 seconds, and the stick runs
       the old one, which it keeps. The new one is still in its bank. Report
       it as an issue of the repository, with the release's version.
   * - "The stick runs from memory (a service start), so updates are not
       available."
     - A start from RAM, over the console: no bank runs (``recovery.rst``).

How long each step takes
========================

.. list-table::
   :header-rows: 1
   :widths: 60 40

   * - Step
     - Time
   * - The install's write, from the shipped firmware's page
     - Not measured
   * - A restart of this system, until the panel answers
     - About half a minute, as the panel says; O5 came about 10 s after the
       kernel started (the test of the first release)
   * - An upload of ``update.bin`` through the panel, written and read back
     - 46 s (``updating.rst``, "Measured on the board")
   * - A trial, from the end of the boot script to the commit, in O5
     - 5 s; given up after 300 s
   * - The copy of the whole flash
     - Not measured through the panel, which allows about a minute; read over
       ssh, 6 s (``backup.rst``)
   * - The shipped firmware's link, down while it tries the lane's modes
     - About a minute at a time
