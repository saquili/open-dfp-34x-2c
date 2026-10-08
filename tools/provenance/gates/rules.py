# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""Regular-expression rules: forbidden words and unit identifiers.

These patterns are generic words and shapes, so they may live in the
repository (docs/policy/provenance.rst, "Gates"). Names of particular parties
never appear here: those are hashed into the deny-list.

Every rule has an id (used in reports and in the allowlist), a pattern and,
where the shape alone is not enough, a check that looks at the match and
decides whether it is a violation. A check returns None for "not a
violation" or a short description of what was found.

Where a forbidden word also has an ordinary technical sense, the rule keeps
that sense out by vocabulary, the way a standards term is kept out: "vendor
ID" (G.984.3), "a memory leak", "make sdk", "an IDA" (the kernel's ID
allocator). Exceptions that depend on *where* a word appears belong in the
allowlist instead.
"""

from __future__ import annotations

import ipaddress
import os
import re
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import GATE_FORBIDDEN, GATE_UNIT_IDS
from .lexer import CODE

# A check sees the match, the kind of segment it sits in and the file path.
Check = Callable[[re.Match, str, str], Optional[str]]


@dataclass(frozen=True)
class Rule:
    gate: str
    id: str
    pattern: re.Pattern
    message: str
    check: Optional[Check] = None
    # Normalised value of a match, for allowlist "values" entries.
    value: Callable[[re.Match], str] = field(default=lambda m: m.group(0).lower())


def _rule(gate: str, rule_id: str, pattern: str, message: str, flags: int = 0,
          check: Optional[Check] = None, value=None) -> Rule:
    kwargs = {}
    if value is not None:
        kwargs["value"] = value
    return Rule(gate, rule_id, re.compile(pattern, flags), message, check, **kwargs)


def _line_around(m: re.Match) -> tuple[str, str]:
    """The text of the match's line before and after the match."""
    text = m.string
    start = text.rfind("\n", 0, m.start()) + 1
    end = text.find("\n", m.end())
    return text[start:m.start()], text[m.end():len(text) if end == -1 else end]


_WORD = re.compile(r"[A-Za-z]+")


def _words_around(m: re.Match, count: int, reach: int = 80) -> tuple[list[str], list[str]]:
    """Up to ``count`` words on either side of a match, across line breaks."""
    text = m.string
    before = _WORD.findall(text[max(0, m.start() - reach):m.start()])[-count:]
    after = _WORD.findall(text[m.end():m.end() + reach])[:count]
    return [w.lower() for w in before], [w.lower() for w in after]


# --------------------------------------------------------------------------
# forbidden words
# --------------------------------------------------------------------------

# Terms of public standards that legitimately follow "vendor": G.984.3
# serial numbers (vendor ID), SFF-8472 fields (vendor name, OUI, PN, rev,
# SN, date code, vendor specific), G.988 attributes (vendor product code),
# device-tree vendor prefixes, DHCP vendor class.
_VENDOR_TERMS = (
    r"ids?|identifiers?|names?|oui|pn|parts?(?:[\s-]+numbers?)?|rev(?:isions?)?|sn|"
    r"serial(?:[\s-]+numbers?)?|specific|date|ssn|prefix(?:es)?|products?|"
    r"extensions?|defined|class(?:es)?"
)
_VENDOR = (
    r"\bvendors?(?:['’]s|s['’])?(?![\w'’])"
    r"(?![\s-]*(?:" + _VENDOR_TERMS + r")\b)"
)


# Another implementation's software named as such, without the word "vendor":
# the firmware or driver a unit came with, or a driver before this one. What a
# reboot leaves behind ("the previous kernel") and the unit's own factory
# configuration are facts of the hardware, not narrative, and stay out.
_OTHER_SOFTWARE = (
    r"\b(?:original|stock|oem|factory)[ \t-]+(?:firmware|drivers?|bootloaders?|software)\b"
    r"|\b(?:previous|old)[ \t-]+drivers?\b"
)


def _strip_c_suffix(m: re.Match) -> str:
    return re.sub(r"[uUlL]+$", "", m.group(0)).lower()


def _ida(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    """"IDA" names a disassembler, and also the kernel's ID allocator."""
    if m.group(0) != "IDA":
        return "the name of a reverse-engineering tool"  # IDA Pro, IDAPython
    before, after = _line_around(m)
    if re.search(r"\ban[ \t]+$", before, re.I):
        return None  # "an IDA": the allocator
    if re.match(r"[ \t-]*(?:allocat\w*|ids?\b|minors?\b|numbers?\b)", after, re.I):
        return None  # "IDA-allocated", "IDA IDs", "IDA minor numbers"
    return "the name of a reverse-engineering tool"


_MAKE_FILE = re.compile(r"^(?:(?:GNU)?[Mm]akefile.*|.*\.mk)$")


def _sdk(m: re.Match, _kind: str, path: str) -> Optional[str]:
    """Refuse a reference to an SDK, except the build system's sdk target."""
    before, after = _line_around(m)
    if re.search(r"\bmake[ \t]+$", before):
        return None  # "make sdk": the distribution build's toolchain export
    if _MAKE_FILE.match(os.path.basename(path)) and m.group(0) == "sdk":
        if re.match(r"[ \t]*\.PHONY[ \t]*:", before):
            return None  # a .PHONY list naming the target
        if not before and re.match(r"[ \t]*:(?!=)", after):
            return None  # the target's own rule
    return "a reference to an SDK"


# A word for source material next to a leak-word makes it a reference to
# leaked material; "a memory leak", "leak the skb" or "leaky bucket" are left
# alone. Words that only ever mean material count within three words either
# side; words with an everyday technical sense only right after the leak-word.
_MATERIAL = {
    "sdk", "sdks", "bsp", "bsps", "datasheet", "datasheets", "trove", "corpus",
    "repo", "repos", "repository", "repositories", "material", "materials", "tarball",
    "tarballs", "archive", "archives", "github", "vendor", "vendors", "documentation",
}
_MATERIAL_NEXT = {"source", "sources", "code", "document", "documents", "table", "tables",
                  "header", "headers", "drop", "docs"}


def _leak(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    before, after = _words_around(m, 3)
    if _MATERIAL & set(before + after) or (after and after[0] in _MATERIAL_NEXT):
        return "a reference to leaked material"
    return None


def _issue(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    if re.search(r",[ \t]*$", _line_around(m)[0]):
        return None  # an assembler immediate operand: "mov r0, #1"
    return "a reference to an issue of a private tracker"


def _corpus(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    before, _after = _words_around(m, 1)
    if before and before[0] in ("fuzz", "fuzzing", "fuzzer", "seed", "test"):
        return None  # a fuzzing or test corpus
    return "a reference to a source corpus"


FORBIDDEN_RULES = (
    _rule(GATE_FORBIDDEN, "re-tool",
          r"decompil|disassembl|ghidra|radare|\br2pipe\b|\bbinary[ \t]+ninja\b|\bhex-?rays\b"
          r"|\breverse[\s-]+engineer",
          "reverse-engineering vocabulary", re.I),
    _rule(GATE_FORBIDDEN, "re-tool", r"\bIDA(?:[ \t]*Pro|Python)?\b",
          "the name of a reverse-engineering tool", check=_ida),
    _rule(GATE_FORBIDDEN, "re-autoname",
          r"\b(?:FUN|DAT|LAB|PTR|UNK)_[0-9A-Fa-f]{6,}\b|\bfcn\.[0-9A-Fa-f]{4,}\b"
          r"|\bsub_[0-9A-Fa-f]{6,}\b",
          "an automatic name made by a disassembler"),
    _rule(GATE_FORBIDDEN, "narrative", _VENDOR,
          "\"vendor\" outside a standards term: narrative about another implementation", re.I),
    _rule(GATE_FORBIDDEN, "narrative", _OTHER_SOFTWARE,
          "another implementation's software: narrative about another implementation", re.I),
    _rule(GATE_FORBIDDEN, "dev-kit", r"(?<![A-Za-z0-9])r?sdks?(?![A-Za-z0-9])",
          "a reference to an SDK", re.I, check=_sdk),
    _rule(GATE_FORBIDDEN, "board-package", r"(?<![A-Za-z0-9])bsps?(?![A-Za-z0-9])",
          "a reference to a board-support package", re.I),
    _rule(GATE_FORBIDDEN, "nonpublic-material", r"\bleak(?:s|ed|ing)?\b",
          "a reference to leaked material", re.I, check=_leak),
    _rule(GATE_FORBIDDEN, "source-collection", r"\bcorp(?:us|ora)\b",
          "a reference to a source corpus", re.I, check=_corpus),
    _rule(GATE_FORBIDDEN, "code-address",
          r"\b0[xX]80[0-9A-Fa-f]{6}(?:[uU]?[lL]{0,2}|[lL]{1,2}[uU])?\b",
          "a kernel-segment code address outside the memory-map allowlist",
          value=_strip_c_suffix),
    _rule(GATE_FORBIDDEN, "issue-reference",
          r"(?<![\w&/#\"'=:.])#\d{1,5}(?![\w-])|\bGH-\d+\b",
          "a reference to an issue of a private tracker", check=_issue),
    _rule(GATE_FORBIDDEN, "lab-path",
          r"/Users/[A-Za-z0-9._-]+"
          r"|/home/[A-Za-z0-9._-]+/(?:git|src|work|projects?|code|dev|Documents|Downloads|Desktop)\b"
          r"|/Volumes/[A-Za-z0-9._-]+|/private/(?:tmp|var)/|~/git/",
          "a laboratory path"),
    _rule(GATE_FORBIDDEN, "internal-reference",
          r"\bsibling[ \t]+(?:repo\w*|session|project|checkout)\b|\blab[ \t]+session\b",
          "a reference to the laboratory's working context", re.I),
)


# --------------------------------------------------------------------------
# unit identifiers
# --------------------------------------------------------------------------

_HEX_LETTERS = set("ABCDEF")


def _gpon_ascii(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    vendor_id = m.group(1)
    if set(vendor_id) <= _HEX_LETTERS:
        return None  # an all-hex run is more likely a number than a serial
    return "a GPON serial number (vendor ID %s)" % vendor_id


def _gpon_hex(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    vendor_id = bytes.fromhex(m.group(1)).decode("ascii")  # the pattern keeps it in 0x40-0x5f
    if not vendor_id.isalpha() or not vendor_id.isupper() or set(vendor_id) <= _HEX_LETTERS:
        return None
    return "a GPON serial number in hexadecimal (vendor ID %s)" % vendor_id


def _mac_octets(text: str) -> list[int]:
    octets = []
    for group in re.split(r"[:.-]", text):
        octets.extend(int(group[i:i + 2], 16) for i in range(0, len(group), 2))
    return octets


def _mac(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    octets = _mac_octets(m.group(0))  # both patterns match exactly six octets
    if all(o == 0 for o in octets) or all(o == 0xFF for o in octets):
        return None  # unspecified, broadcast
    if octets[0] & 1:
        return None  # group (multicast) address
    if octets[:5] == [0x00, 0x00, 0x5E, 0x00, 0x53]:
        return None  # RFC 7042 documentation unicast range
    return "a MAC address outside the documentation, multicast and broadcast ranges"


def _nets(*cidrs: str) -> list[ipaddress.IPv4Network]:
    return [ipaddress.ip_network(c) for c in cidrs]


_DOC_V4 = _nets("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24")
# Loopback, "this network", link-local, multicast, reserved.
_SPECIAL_V4 = _nets("127.0.0.0/8", "0.0.0.0/8", "169.254.0.0/16", "224.0.0.0/4", "240.0.0.0/4")
_PRIVATE_V4 = _nets("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10")
# Writing one of these blocks whole, in CIDR form, names the block, not a host.
_WHOLE_BLOCKS = set(_DOC_V4 + _SPECIAL_V4 + _PRIVATE_V4)


# A standards clause has the shape of an address ("G.984.3 9.2.3.1"); the
# policy asks for clause citations, so a number right after a standard's
# designator or a clause word is a clause, not an address.
_CLAUSE_BEFORE = re.compile(
    r"(?:\b(?:clauses?|sub-?clauses?|sections?|sec\.|tables?|figures?|fig\.|annex|appendix|"
    r"paragraphs?|para\.|items?)|§|\b(?:[GIXY]\.\d{3,4}(?:\.\d+)?|802\.\d+\w*|1588|"
    r"SFF-\d{4}|INF-\d{4}|RFC[ \t]?\d+)(?:-\d{4})?(?:[ \t]*,?[ \t]*(?:clause|section|§))?)"
    r"[ \t]*$", re.I)


def _is_netmask(addr: ipaddress.IPv4Address) -> bool:
    value = int(addr)
    inverted = value ^ 0xFFFFFFFF
    return (inverted + 1) & inverted == 0  # ones followed by zeros


def _ipv4(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    try:
        addr = ipaddress.IPv4Address(m.group(1))
    except ValueError:
        return None
    if _CLAUSE_BEFORE.search(_line_around(m)[0]):
        return None
    if m.group(2) is not None and int(m.group(2)) <= 32:
        block = ipaddress.ip_network("%s/%s" % (addr, m.group(2)), strict=False)
        if block.network_address == addr and block in _WHOLE_BLOCKS:
            return None
    if _is_netmask(addr) or any(addr in net for net in _DOC_V4 + _SPECIAL_V4):
        return None
    if any(addr in net for net in _PRIVATE_V4):
        return "a private IPv4 address (use the RFC 5737 documentation ranges)"
    return ("a public IPv4 address (use the RFC 5737 documentation ranges; cite a standards "
            "clause as \"G.984.3 9.2.3.1\" or \"clause 9.2.3.1\")")


_DOC_V6 = ipaddress.ip_network("2001:db8::/32")
_LINK_LOCAL_V6 = ipaddress.ip_network("fe80::/10")
_MULTICAST_V6 = ipaddress.ip_network("ff00::/8")


def _ipv6(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    text = m.group(1)
    if "::" not in text and text.count(":") != 7:
        return None
    groups = [g for g in text.split(":") if g]
    if len(groups) < 2 or max(len(g) for g in groups) < 3:
        return None  # "a::b", "1::2" -- more likely code than an address
    try:
        addr = ipaddress.IPv6Address(text)
    except ValueError:
        return None
    if addr in _DOC_V6 or addr in _MULTICAST_V6 or addr.is_loopback or addr.is_unspecified:
        return None
    if addr in _LINK_LOCAL_V6 and int(addr) & ((1 << 64) - 1) < 0x10000:
        return None  # link-local with a small, hand-written interface ID
    return "an IPv6 address outside the RFC 3849 documentation prefix"


_PLACEHOLDER = re.compile(r"^(?:[<{$%@].*|x+|X+|\*+|0+|\.\.\.|changeme|example\w*|secret|"
                          r"password|passwd|your\w*|none|null|empty|redacted)$", re.I)

_CONFIG_SUFFIXES = (".yaml", ".yml", ".toml", ".ini", ".conf", ".cfg", ".config", ".env", ".json")


def _literal(quote: str, value: str, kind: str, path: str) -> bool:
    """Is an assigned value a literal rather than an expression in code?"""
    if quote or kind != CODE or path.endswith(_CONFIG_SUFFIXES):
        return True
    return re.fullmatch(r"(?:0[xX])?[0-9A-Fa-f]{8,}", value) is not None


def _device_serial(m: re.Match, kind: str, path: str) -> Optional[str]:
    quote, value = m.group(1), m.group(2)
    if _PLACEHOLDER.match(value) or not re.search(r"\d", value):
        return None
    if not _literal(quote, value, kind, path):
        return None
    return "a device serial number"


def _password(m: re.Match, kind: str, path: str) -> Optional[str]:
    quote, value = m.group(1), m.group(2)
    if _PLACEHOLDER.match(value) or not _literal(quote, value, kind, path):
        return None
    return "a password or registration secret"


# Host names. A laboratory's machines have names as well as addresses, and a
# name is as private as the address it stands for. Two shapes are refused: a
# name under a top-level name that the public DNS does not delegate or that
# sites keep for their own networks (RFC 6762 .local, RFC 8375 home.arpa and
# the customary private ones); and a name given as the host of a URL in a
# setting (target, host, proxy, server, endpoint, a base or API URL) or as the
# host of a remote login or copy (ssh, scp, sftp, rsync, telnet). The names
# RFC 2606 and RFC 6761 keep for documentation and testing are left alone,
# and so are addresses, which the address rules check.
_LABEL = r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
_HOSTNAME = r"(?P<host>%s(?:\.%s)+)" % (_LABEL, _LABEL)
_HOST_END = r"(?![\w-]|\.[A-Za-z0-9])"
_PRIVATE_TLD = r"(?:lan|home|home\.arpa|internal|intranet|corp|localdomain|local|lab|private)"
_DOC_HOST = re.compile(r"(?:^|\.)(?:example\.(?:com|net|org)|example|test|invalid|localhost)$",
                       re.I)


def _host_value(m: re.Match) -> str:
    return m.group("host").lower()


# Script sources the lexer does not split: there a name is taken as a host only
# where a host is written, after a quote, a slash or an at sign.
_SCRIPT_SOURCE = re.compile(r"\.(?:[cm]?js|jsx|ts|tsx|vue)$")


def _private_host(m: re.Match, kind: str, path: str) -> Optional[str]:
    if kind == CODE:
        return None  # a structure member called local or internal
    before = m.string[m.start() - 1:m.start()]
    if _SCRIPT_SOURCE.search(path) and before not in ("\"", "'", "`", "/", "@"):
        return None  # a property chain of a script, ending in a property called lan
    return "a host name under a private top-level name"


def _named_host(m: re.Match, _kind: str, _path: str) -> Optional[str]:
    host = m.group("host")
    if _DOC_HOST.search(host) or not re.search(r"[A-Za-z]{2,}$", host):
        return None  # a documentation name, or an address or version number
    return "a host name (use the RFC 2606 names: example.com, *.example, *.test)"


UNIT_ID_RULES = (
    _rule(GATE_UNIT_IDS, "host-name",
          r"(?<![\w.-])(?P<host>(?:%s\.)+%s)%s" % (_LABEL, _PRIVATE_TLD, _HOST_END),
          "a host name", re.I, check=_private_host, value=_host_value),
    _rule(GATE_UNIT_IDS, "host-name",
          r"(?<![A-Za-z0-9])(?:target|host(?:name)?|proxy|server|endpoint|(?:base|api)[_-]?url)"
          r"(?![A-Za-z0-9])[\"']?[ \t]*[:=][^\n]{0,80}?"
          r"(?<![A-Za-z0-9+.-])[A-Za-z][A-Za-z0-9+.-]*://"
          r"(?:[^\s\"'`/@]+@)?" + _HOSTNAME + _HOST_END,
          "a host name", re.I, check=_named_host, value=_host_value),
    _rule(GATE_UNIT_IDS, "host-name",
          r"(?<![\w-])(?:ssh|scp|sftp|rsync|telnet)(?![\w-])[^\n]{0,120}?[\w.-]+@"
          + _HOSTNAME + _HOST_END,
          "a host name", re.I, check=_named_host, value=_host_value),
    _rule(GATE_UNIT_IDS, "gpon-serial",
          r"(?<![A-Za-z0-9])([A-Z]{4})-?([0-9A-Fa-f]{8})(?![A-Za-z0-9])",
          "a GPON serial number", check=_gpon_ascii),
    _rule(GATE_UNIT_IDS, "gpon-serial",
          r"(?<![0-9A-Za-z])(?:0[xX])?((?:[4-5][0-9A-Fa-f]){4})([0-9A-Fa-f]{8})(?![0-9A-Za-z])",
          "a GPON serial number", check=_gpon_hex),
    _rule(GATE_UNIT_IDS, "device-serial",
          r"\b(?:s/n|sn|serial(?:[ \t_-]*(?:no\.?|num(?:ber)?))?)[ \t]*[:=#][ \t]*([\"']?)"
          r"([A-Za-z0-9][A-Za-z0-9-]{5,})",
          "a device serial number", re.I, check=_device_serial, value=lambda m: m.group(2).lower()),
    _rule(GATE_UNIT_IDS, "mac-address",
          r"(?<![0-9A-Fa-f:.-])[0-9A-Fa-f]{2}([:-])(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}"
          r"(?![0-9A-Fa-f]|[:.-][0-9A-Fa-f])",
          "a MAC address", check=_mac),
    _rule(GATE_UNIT_IDS, "mac-address",
          r"(?<![0-9A-Fa-f.])[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}(?![0-9A-Fa-f]|\.[0-9A-Fa-f])",
          "a MAC address", check=_mac),
    _rule(GATE_UNIT_IDS, "ipv4-address",
          r"(?<![\w.])(\d{1,3}(?:\.\d{1,3}){3})(?:/(\d{1,2}))?(?![\w/]|\.\d)",
          "an IPv4 address", check=_ipv4, value=lambda m: m.group(1)),
    _rule(GATE_UNIT_IDS, "ipv6-address",
          r"(?<![\w:.])((?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4})(?:/\d{1,3})?(?![\w:.])",
          "an IPv6 address", check=_ipv6, value=lambda m: m.group(1).lower()),
    _rule(GATE_UNIT_IDS, "password",
          r"\b(?:password|passwd|passphrase|pwd|ploam[ \t_-]*pw|registration[ \t_-]*id|loid)\b"
          r"[\"']?[ \t]*[:=][ \t]*([\"']?)([^\s\"',;()\[\]{}]{4,})\1",
          "a password", re.I, check=_password, value=lambda m: m.group(2).lower()),
)

ALL_RULES = FORBIDDEN_RULES + UNIT_ID_RULES
