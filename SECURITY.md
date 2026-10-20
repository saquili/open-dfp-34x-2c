<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> -->

# Security

The stick sits between an operator's fibre and a router, with an ssh server
and a web panel on the router's side. Please report a weakness privately
before you make it public.

## How to report

* Through GitHub: the repository's **Security** tab, **Report a
  vulnerability**, which opens a private advisory.
* Or by e-mail to Paweł Phan <szop.hektar_1w@icloud.com>.

Say what is affected (the program, and the commit or the build), how to
reproduce it, and what it lets an attacker do. Leave out your stick's serial
numbers, MAC addresses, registration password and captures of your
operator's line: a report does not need them, and the project keeps them out
of the repository (`docs/policy/provenance.rst`).

## What to expect

One person maintains the project. You get an answer as soon as they can give
one; a fix goes into the tree and into the next update file, and the
advisory names the commit that has it. Releases are test builds, made by the
repository's release workflow and listed with their SHA-256 digests
(`README.rst`, "Releases").

## Scope

* In scope: the code of this repository -- the kernel series under `linux/`,
  omcid, onu-utils, pond and its panel, pppoed, dhcp6d, timed, netifd as
  carried here, and the build and its update files.
* Known and documented, so not a new finding: update files carry a checksum
  but no signature (ADR 0014); the panel's certificate is signed by the stick
  itself (ADR 0013); the one account takes the shipped firmware's
  administrator password at the first start, and while it is the default,
  `admin`, the panel allows nothing but a change and ssh takes keys only
  (ADR 0009). The stick's own servers are to answer on its management VLAN
  alone, in every mode; a way to them from anywhere else is a finding. The
  guard that keeps them so is not yet tried on a stick (`README.rst`, "Not
  done yet").
* Not in scope: the shipped firmware, the boot loader and the operator's
  equipment.
