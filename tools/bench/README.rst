.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===========
tools/bench
===========

The serial-console bench for bring-up and flashing the DFP-34X-2C boards.

It drives the board through its shared console: it breaks into the boot
loader, boots a trial kernel from RAM over TFTP (``ramboot``), and writes an
image to bank 0, the scratch bank, behind a **bank guard** that refuses every
command that could write anything else (``flash``). It needs Python 3.11 or
later and nothing outside its standard library. The unit tests need no board,
no network and no tmux; a maintainer validates the tool on the real board
afterwards (see `What is checked only on the board`_).

Layout
======

``tools/bench`` is the Python package ``bench``; its parent, ``tools``, is where
it is run from. The modules, in the order in which each imports only the ones
before it:

``guard.py``
    The bank guard: the allowlist of loader lines, the flash map, the macro
    preflight, the target-word and bank-number rules, the session rules.
``console.py``
    The shared console: typing by tmux ``send-keys``, reading the transcript by
    byte offset, waits, the usable-check, the exclusive lock.
``client.py``
    The process that a tmux pane runs to connect a terminal to the UART bridge.
``image.py``
    Checks of a kernel image and of a root filesystem image, and of what the
    loader prints when it starts a kernel.
``uboot.py``
    The loader driver: break-in, guarded typing with the echo check, the
    environment, saving it.
``tftp.py``
    A read-only TFTP server for one directory (stands alone; it is copied to the
    image host and run there).
``host.py``
    The command runner, the image host (the served file, the server, the
    firewall allowance) and the ssh lines to the board.
``ramboot.py``
    The RAM-boot sequence, with its markers, timeouts and one recovery.
``rawport.py``
    Raw ports to the UART (TCP to the bridge, or a serial device) and a console
    over one.
``ymodem.py``
    A YMODEM sender with CRC-16 that works on any object with ``read`` and
    ``write``.
``flash.py``
    The write procedure for ``k0`` and ``r0``, the one-shot trial and its
    fallback, the plan that a dry run prints.
``settings.py``, ``cli.py``, ``__main__.py``
    Configuration and the command line.

``tests/`` holds the unit tests and what they run against: a virtual clock, a
tmux double, a scripted board (countdown, prompt, commands, a network, a
kernel, a watchdog, an 8 MiB flash, a YMODEM receiver) and a command-runner
double that acts as an image host. The scripted board's variables have the
board's shape, partition tables with parentheses included, and
``tests/test_board_environment.py`` runs the guard and the whole of RAM boot
and flashing on macros as long as the board's. ``tests/vectors.py`` holds every
verdict of the guard as data.

Running
=======

From the ``tools`` directory::

    python3 -m bench COMMAND ...

The unit tests, also from ``tools``::

    python3 -m unittest discover -s bench/tests -t .

or from the repository root::

    python3 -m unittest discover -s tools/bench/tests -t tools

or ``python3 -m bench selftest``. The whole suite takes under a minute. ``BENCH_MUTATIONS`` sets the number of
mutated lines the guard's invariant test tries (400000 by default; a million
takes about ten seconds).

The top-level ``make ramboot`` and ``make flash BANK=0`` are meant to call
``python3 -m bench ramboot`` and ``python3 -m bench flash --bank $(BANK)``.

Setting up
==========

**The console.** The board's UART is reached through a network bridge that
serves one client per port. That client runs in a tmux pane on the default tmux
server (a named socket would hide the session from a person), whose output is
mirrored into an append-only file. For the board under development::

    tmux new-session -d -s uart-wifi-1 -c TOOLS_DIRECTORY \
        'python3 -m bench --set console.bridge_host=BRIDGE console client'
    tmux set-option -t uart-wifi-1 remain-on-exit on
    tmux set-option -t uart-wifi-1 history-limit 200000
    tmux pipe-pane -t uart-wifi-1 'cat >> TRANSCRIPT'

Run ``pipe-pane`` without ``-o``: with ``-o`` a second run switches the logging
off. ``console.transcript`` names the file; the default is
``~/.local/state/bench/<session>.log``. ``python3 -m bench console check`` says
whether the console is usable and what the board shows. A person may watch and
type in the same pane (``tmux attach -t uart-wifi-1``); the tool types keys
into the pane and reads the transcript, and never reads the screen.

**The image host** carries the SFP cage the board sits in and serves the image
over TFTP; it may be the machine the tool runs on (``host.ssh = "local"``). It
needs Linux with ``ss``, ``nft``, ``sudo`` without a password for the commands
the tool runs, and Python 3.8 or later for the server. Set ``host.interface``,
``host.bind`` (the address of the cage's interface) and the directories.

**The board** needs, once, set by a person at the loader prompt and saved:
``ipaddr``, ``serverip`` and ``netmask`` (they must equal the
``board.loader_*`` settings), and the macros ``sdsup`` and ``ramboot``. The
tool never sets them and stops with the missing variable's name instead.

Commands
========

Every command that acts on a board takes ``--dry-run``: it prints the exact
lines it would type, each already approved by the guard, and touches neither
the console nor the image host.

``guard-check LINE...`` (alias ``guard``)
    Says whether the guard lets each line through; sends nothing. Without
    ``--var`` only the line itself is judged. ``--var NAME=VALUE`` (repeatable)
    gives the board's variables, as ``printenv`` prints them; the macros that
    ``run`` and ``boot`` would execute are then preflighted too, as on a
    console, and a macro that is not given is refused with ``missing-macro``.
    ``--commit`` sets commit mode, ``--read-only`` judges for a read-only
    console, ``--target WORD`` and ``--bank N`` judge a target word or a bank
    number. The lines are judged in order in one session, so
    ``guard-check 'setenv sw_tryactive 0' saveenv saveenv`` allows two lines and
    refuses the third.

``ramboot [IMAGE]``
    Boots the kernel image from RAM. Writes nothing to flash. ``IMAGE`` defaults
    to ``build/linux/arch/mips/boot/uImage`` under the repository root.
    ``--ready PATTERN`` is the image's own ready marker (a regular expression,
    ``login:`` by default), ``--ready-limit SECONDS`` its limit, counted from
    the typed ``run ramboot`` (60), ``--recovery fuse|reset`` how a failed first
    attempt is recovered (see below).

``flash --target kernel|rootfs [--bank 0] [--via tftp|ymodem] [--arm] [--commit] IMAGE``
    Writes ``k0`` (a kernel image) or ``r0`` (a SquashFS image) of bank 0,
    verifies it, and checks that bank 1 did not change. ``--arm`` then arms a
    one-shot trial of bank 0 and boots it; ``--commit`` makes bank 0 the
    committed bank (``sw_commit`` 0). ``--accept-same-bank-fallback`` allows
    arming while ``sw_commit`` is 0, so that a failed trial returns to bank 0
    again. On a bridge console the transport is TFTP: YMODEM through the bridge
    is refused unless ``--ymodem-over-bridge`` is given and ``console.bridge_host``
    names the bridge, because it has not been tried (it stops the pane's client
    and opens the one connection itself). With ``console.serial_device`` set,
    YMODEM uses that port directly.

``serve start|check|stop|run|remove-rule``
    The image host's server. ``start [--image PATH] [--name NAME]`` pushes the
    image, restarts the server from the current source and adds the firewall
    allowance if it is not there; ``check`` reports the served file, the server
    and the allowance; ``stop [--remove-rule]``; ``run`` serves in the
    foreground on this machine.

``console check|type LINE|client``
    ``check`` decides whether the console is usable and says what the board
    shows. ``type LINE [--timeout SECONDS]`` types one line through the guard
    (only at the loader prompt) and waits for the prompt, 30 seconds by default.
    ``client`` runs the pane's process, for the setup above.

``selftest [--verbose] [--pattern GLOB]``
    Runs the unit tests (``--pattern test_guard*.py`` runs a part).

Exit status: 0 success; 1 the operation failed; 2 usage; 3 a guard refusal,
reported on standard error as ``refused [<code>] <detail>: <line>`` with
nothing typed; 4 the console is not usable; 5 the board is in an unexpected
state; 6 a timeout.

A RAM boot, in order: the image is checked (the legacy header with its CRCs, MIPS,
LZMA, load address, at most 12 MiB); the console is checked and locked; the
image, the server and the firewall allowance are made ready on the image host;
the board is brought to the loader prompt (by SysRq ``b`` over ssh if it is
running, by waiting for a person's power cycle if ssh cannot reach it); the
environment is read and the macros and addresses checked; ``run ramboot`` is
typed and the transfer watched; the loader's report of the image must be the
file's (name, creation time, size, addresses); the kernel's banner and then the
ready marker must follow. A restart through the CPU reset vector leaves the
watchdog counting, and about 47 seconds later it resets the chip. After such a
restart the first transfer sometimes shows only ``T`` marks; two of them without
a ``#`` fail the attempt, Ctrl-C is pressed, and the tool waits for that watchdog
reset (``--recovery fuse``, the default, which types ``reset`` at the prompt if
no watchdog reset has come within a minute) or types ``reset`` at once
(``--recovery reset``, expected to be about 35 seconds faster, and waiting for
the watchdog reset if the prompt does not come back) and breaks in again. One
recovery is made per run.

A flash, in order: the file is checked for its partition and its CRC-32 taken;
the loader prompt is reached; the layout variables (``fl_*`` and the two kernel
window addresses) must equal the tool's table and ``sw_tryactive`` must read 2;
the CRC-32 of bank 1 is recorded through the loader's flash window; the image
reaches RAM at ``0x80000000`` by TFTP (``run sdsup`` first) or by YMODEM, and its
CRC-32 there must equal the host's; the watchdog is stopped (``mw.l b8003268 0``),
after that check for TFTP, where the reset heals a failed first attempt, and
before the transfer for YMODEM; the partition is erased and written; the flash read back through the window must
give the host's CRC-32; bank 1's checksums must not have changed. Only then, if
asked, are ``sw_tryactive`` (and ``sw_commit``) set and saved, and the trial
booted and watched: the loader's report must be the flashed kernel at the bank 0
window, then the banner and the ready marker, and ``root=31:5`` in
``/proc/cmdline`` when ssh can reach the board.

YMODEM through the bridge replaces the pane's process for the length of the
transfer and puts the original back when it ends, whatever the outcome; a
Ctrl-C or ``SIGTERM`` meanwhile is held until then. If the tool is killed
outright (``SIGKILL``, a power loss on the tool host), the pane is left running
the placeholder: restore it by hand with ``tmux respawn-pane -k -t SESSION
'COMMAND'``, COMMAND being the client's command from the setup above, and
``tmux set-option -t SESSION remain-on-exit on``.

Configuration
=============

Every value has a default and an override. Values come from the defaults, then a
TOML file (``--config FILE`` or ``BENCH_CONFIG``), then ``--set KEY=VALUE``
(repeatable; a value that is not TOML is taken as text). The addresses are the
documentation ranges. Unknown keys and values of the wrong type are errors.

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - key
     - default and meaning
   * - ``console.session``
     - ``uart-wifi-1``: the tmux session of the console
   * - ``console.transcript``
     - a per-user file named after the session
   * - ``console.tmux_socket``
     - empty: the default tmux server
   * - ``console.read_only``
     - true for ``uart-wifi-2`` (the reference board), else false
   * - ``console.bridge_host``
     - empty: the UART bridge's host
   * - ``console.bridge_port``
     - 2001: the UART bridge's TCP port
   * - ``console.serial_device``
     - empty: a serial device, for a direct YMODEM console
   * - ``host.ssh``
     - ``local``, or an ssh destination of the image host
   * - ``host.ssh_options``
     - empty list: ssh options, for example a jump host
   * - ``host.served_dir``
     - ``/tmp/bench/images``: holds only the images
   * - ``host.work_dir``
     - ``/tmp/bench``: the server's source, pid file and log
   * - ``host.interface``
     - ``eth1``: the cage's interface, which the allowance names
   * - ``host.bind``
     - ``192.0.2.1``: the one address the server binds
   * - ``host.port``
     - 69
   * - ``host.rule_tag``
     - ``bench-tftp``: the allowance's comment
   * - ``host.sudo``
     - true: port 69 and the firewall need root
   * - ``board.control_address``
     - ``198.51.100.2``: tried first for ssh to the board
   * - ``board.data_address``
     - ``192.0.2.2``: tried second
   * - ``board.user``
     - ``root``
   * - ``board.jump``
     - empty: an ssh destination to reach the board through
   * - ``board.jump_options``
     - empty list: ssh options for that hop
   * - ``board.loader_ipaddr``
     - ``192.0.2.2``: what the loader's ``ipaddr`` must read
   * - ``board.loader_serverip``
     - ``192.0.2.1``: what ``serverip`` must read
   * - ``board.loader_netmask``
     - ``255.255.255.252``: what ``netmask`` must read
   * - ``image.kernel_name``
     - ``uImage``: the served name of the kernel
   * - ``image.rootfs_name``
     - ``rootfs``: the served name of the root filesystem
   * - ``image.squashfs_compressions``
     - ``[1, 5]``: the SquashFS compression ids the kernel takes
   * - ``boot.ready_pattern``
     - ``login:``: the image's ready marker, a regular expression
   * - ``boot.ready_limit``
     - 60: seconds from the typed ``run ramboot`` (from ``boot`` for a trial)
   * - ``boot.recovery``
     - ``fuse``: wait for the watchdog reset, and type ``reset`` if it does not
       come; or ``reset``: type ``reset`` first
   * - ``log.path``
     - empty: the tool's own timestamped log

What the tool refuses, and why
==============================

The loader executes whatever arrives, so the tool never relies on it to refuse
anything. Every line passes the guard before a byte is typed, and a refusal
sends nothing.

* **Only ``k0`` and ``r0`` are written**, each only by an image made for it.
  The loader (``boot``), its two environment copies (``env``, ``env2``), the
  configuration store (``config``) and the shipped firmware of bank 1 (``k1``,
  ``r1``) are protected: a damaged ``boot`` partition is assumed to be beyond
  repair from the loader, ``config`` holds the device's identity, and bank 1 is
  the fallback. The command line takes a target word (``kernel``, ``rootfs``) and
  a bank number, never an offset; ``--bank 1`` and a protected name are refused
  before any work.
* **An allowlist, not a blocklist.** A line that is not one of the listed forms
  is refused, however harmless it looks. Command names are compared exactly (the
  loader may accept abbreviations); numbers are hexadecimal, at most 32 bits;
  in a typed line quotes, backquotes, backslashes, ``;``, ``&``, ``|``,
  parentheses, braces, redirections and ``$(`` are refused outright, so a line
  is one command and a variable's value can neither add another nor carry a
  quote that hides one. Variables in a typed
  line are expanded from the environment read when the session began (a line
  typed on its own reads the variables it names from the board).
* **Ranges are checked as numbers**, after variables are expanded: a write must
  lie wholly inside ``k0`` or wholly inside ``r0``, with no overlap with a
  protected partition, no zero length, no end beyond the chip.
* **The flash windows are never written**: a store into the memory-mapped
  windows (physical ``0x14000000`` and the reset-vector window, in every
  spelling) is refused for every command. The loader's window is read only.
* **Macros are checked by what they are now.** ``run`` is allowed only for
  ``ramboot``, ``sdsup`` and ``b0``, and ``boot`` only after its boot chain is
  checked; the text of each macro, and every variable it names, is read from the
  board at that moment (not from the copy read at the start) and must consist of
  forms that cannot write the flash (no loops, no conditionals outside the boot
  chain, no quotes, stores only into the register window). The board's own
  flashing macros are never run. A part of a macro is judged as a typed line is,
  with one exception: in a part that begins ``setenv bootargs`` the parentheses
  that come from the *value* of a variable are accepted, because the partition
  tables the kernel arguments are made of hold them (``256K(boot)``) and
  ``setenv`` stores its words as data. The part's own text, and every other form
  (``run``, ``tftpboot``, ``bootm``, the register writes, the chain's comparison
  and saves, a ``setenv`` of another variable), refuses a parenthesis from any
  source; and a value that holds any other of ``; & | { } < > $``, or a quote,
  backquote or backslash, is refused wherever it is used. A refusal names the macro that held the text.
* **Session rules.** ``saveenv`` only after a ``setenv`` the guard allowed;
  ``sw_tryactive`` only to 0 or 2 (a trial of bank 1 is never armed and ``boot``
  is refused while one is pending); ``sw_commit`` only to 0, and only on an
  explicit ``--commit``; no other variable is ever set.
* **A write is never interrupted.** Nothing is typed while ``loady``,
  ``sf erase`` or ``sf write`` runs; a signal is held until the command is done;
  a console that stalls for 30 seconds mid-write ends the run, says the
  partition is invalid and arms nothing. Ctrl-C is never sent into those
  commands.
* **A trial is armed only after the flash read back as the file**, bank 1's
  checksums are unchanged, and ``sw_tryactive`` read 2 at the start. When
  ``sw_commit`` is 0 a failed trial returns to the same bank, so arming needs
  ``--accept-same-bank-fallback``.
* **The console is shared.** Keys are typed into the pane and the transcript is
  read by byte offset: never the screen (``capture-pane``), never a second
  connection to the bridge (it would replace the first or merge its input onto
  the UART), never a kill of the session, never ``pipe-pane -o``, never a
  truncated transcript, never a BREAK. Only an empty line, Enter or Ctrl-C is
  typed anywhere but at the loader prompt, and a login prompt is never answered.
  Two operations at once are refused (a lock next to the transcript).
* **Success is read from output after the action** that provokes it, never from
  a marker that was already there, from a search of the whole transcript, or from
  a ready marker alone; a different kernel than the one sent is a failure.
* **The image host is touched as little as possible**: one file (replaced by a
  rename), the server's own source, and one firewall rule for UDP 69 on the
  cage's interface with a comment that names it. The cage's interface is never
  brought up or down, its address never changed, its network service never
  restarted. The server serves one directory read-only on one address, refuses
  writes, any name that is not a plain file name, and anything that is not a
  regular file (a symbolic link included).
* **The environment dump is never kept or printed**: it holds a MAC address, and
  every console tail and log line is redacted.

What is checked only on the board
=================================

The unit tests cover the logic against simulations. These behaviours rest on
the board, the loader and the hosts, and are for the maintainer to confirm:

* the real loader's parsing: the guard assumes the worst (abbreviations,
  numbers read modulo 2**32, expansion split at separators), which no test of a
  simulation can confirm;
* tmux behaviour: the calls ``pipe-pane`` and ``respawn-pane`` on the tmux
  version in use, a key typed during a bridge flap, the pane's start command as
  ``#{pane_start_command}`` reports it;
* the first transfer after a restart: that it fails as described, that Ctrl-C
  and ``reset`` behave as expected (``--recovery reset``), and that the watchdog
  reset comes about 47 seconds after the restart; and that ``mw.l b8003268 0``
  at the prompt stops that count on the current kernel (a flash that starts
  after a restart is cut off by the reset if it does not);
* the TFTP server against the loader: one address, one directory, a fresh port
  per transfer, ``blksize`` 1468;
* flashing by TFTP into ``k0``, that the lane survives the erase and the write,
  the durations of ``sf erase``, ``sf write``, ``saveenv`` and ``crc32`` (the
  stall limit is 30 seconds without a new byte), and that ``crc32`` through the
  window equals a host CRC-32 for both RAM and flash;
* YMODEM through the bridge (stopping the pane's client, one raw connection,
  getting the console back) and what the loader prints after a cancel;
* ``boot`` as the way to start a trial, and ``root=31:5`` over ssh;
* the pane's client in a real pane: reconnects, the raw terminal, detaching;
* on the image host: ``ss``, ``nft`` (the ``inet filter input`` chain), password-
  less ``sudo``, and Python 3.8 or later for the server.

The defaults for the second UART's console being read-only, and the suggested
command line itself, are choices of this tool, not facts about the board.
