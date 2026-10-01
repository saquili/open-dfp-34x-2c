# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
"""A read-only TFTP server for one directory of images.

The boot loader fetches its images over TFTP. The server here serves one
directory, read-only, to that client: RFC 1350 with the option extension of
RFC 2347 and the ``blksize`` (RFC 2348) and ``tsize`` (RFC 2349) options. It
is the tool's own, written against the standard library only and run on a
single thread, so that the file can be copied to the image host and started
there with nothing but a Python 3 interpreter; this module therefore imports
nothing from the rest of the package and avoids syntax newer than Python 3.8.

What it refuses is as much of its design as what it serves. A write request
is refused. A name that is not a plain file name (a character outside
letters, digits, ``.``, ``_`` and ``-``, a leading dot, a path separator,
``.`` or ``..``, empty, or longer than 64 characters) is refused, and so is
anything that is not a regular file directly in the served directory, a
symbolic link included. Every transfer runs from a fresh UDP socket on an
ephemeral port, as the loader expects; replies, errors included, never come
from the listening port. A server that keeps answering from the port it
listens on works with some clients and not with the loader. The server binds
the one address it is given, never every address.

Where the loader's behaviour is not known the server keeps to the standard:
options it does not know are left out of the option acknowledgement, an
option with an unusable value is ignored, and an acknowledgement from any
address but the client's is dropped. Binding the one address and serving one
directory is the narrowest setup that can work; it has not been tried against
every loader, so a server that is reached on no address is the first thing to
look at when a transfer shows only ``T`` marks.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import select
import signal
import socket
import stat
import struct
import sys
import time
from typing import BinaryIO, Callable, Dict, List, Optional, Tuple

OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR, OP_OACK = 1, 2, 3, 4, 5, 6

ERR_NOT_FOUND = 1
ERR_ACCESS = 2
ERR_ILLEGAL = 4

DEFAULT_BLKSIZE = 512
MIN_BLKSIZE = 8
MAX_BLKSIZE = 8192
RETRIES = 5  # a data block is resent this many times before the transfer is given up
TIMEOUT = 3.0  # seconds between resends
MAX_NAME = 64
_PLAIN_NAME = re.compile(r"[A-Za-z0-9._-]{1,%d}" % MAX_NAME)

Address = Tuple[str, int]


class ProtocolError(Exception):
    """A request the server answers with an error packet."""

    def __init__(self, code: int, text: str) -> None:
        super().__init__(text)
        self.code = code
        self.text = text


def error_packet(code: int, text: str) -> bytes:
    """An error packet: opcode 5, a two-byte code and a text ending in NUL."""
    return struct.pack(">HH", OP_ERROR, code) + text.encode("ascii", "replace") + b"\0"


class Request:
    """A parsed read request."""

    def __init__(self, filename: str, mode: str, options: Dict[str, str]) -> None:
        self.filename = filename
        self.mode = mode
        self.options = options


def parse_request(packet: bytes) -> Request:
    """Check a request packet and parse it, or raise the error it deserves.

    A write request is refused first. Any other request is
    ``name NUL mode NUL`` followed by ``option NUL value NUL`` pairs; names
    and options compare in lower case.
    """
    if len(packet) < 2:
        raise ProtocolError(ERR_ILLEGAL, "illegal request")
    opcode = struct.unpack(">H", packet[:2])[0]
    if opcode == OP_WRQ:
        raise ProtocolError(ERR_ACCESS, "read-only server")
    if opcode != OP_RRQ:
        raise ProtocolError(ERR_ILLEGAL, "illegal operation")
    body = packet[2:]
    if not body.endswith(b"\0"):
        raise ProtocolError(ERR_ILLEGAL, "illegal request")
    parts = body[:-1].split(b"\0")
    if len(parts) < 2 or len(parts) % 2:
        raise ProtocolError(ERR_ILLEGAL, "illegal request")
    words = [part.decode("latin-1") for part in parts]
    name, mode = words[0], words[1].lower()
    if not _PLAIN_NAME.fullmatch(name) or name.startswith(".") or "/" in name:
        raise ProtocolError(ERR_ACCESS, "invalid file name")
    if mode not in ("octet", "netascii"):
        raise ProtocolError(ERR_ILLEGAL, "unsupported mode")
    options = {}  # type: Dict[str, str]
    for key, value in zip(words[2::2], words[3::2]):
        options.setdefault(key.lower(), value)
    return Request(name, mode, options)


def to_netascii(data: bytes) -> bytes:
    """Convert octets to netascii: a line feed becomes CR LF, a carriage return CR NUL."""
    out = bytearray()
    for byte in data:
        if byte == 0x0A:
            out += b"\r\n"
        elif byte == 0x0D:
            out += b"\r\0"
        else:
            out.append(byte)
    return bytes(out)


def open_regular(directory: str, name: str, mode: str) -> Tuple[BinaryIO, int]:
    """Open a regular file that lies directly in ``directory``, or raise the error.

    A symbolic link, a directory or anything else that is not a regular file
    is "no such file". The open never follows a link.
    """
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(os.path.join(directory, name), flags)
    except FileNotFoundError:
        raise ProtocolError(ERR_NOT_FOUND, "no such file")
    except PermissionError:
        raise ProtocolError(ERR_ACCESS, "access denied")
    except OSError:
        raise ProtocolError(ERR_NOT_FOUND, "no such file")
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise ProtocolError(ERR_NOT_FOUND, "no such file")
        handle = os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise
    if mode == "netascii":
        with handle:
            converted = to_netascii(handle.read())
        return io.BytesIO(converted), len(converted)
    return handle, info.st_size


class Transfer:
    """One read transfer, as a state machine that does no input or output.

    ``first()`` gives the opening packet. ``on_packet`` takes what the client
    sent and ``on_timeout`` the passing of ``TIMEOUT`` seconds; each answers
    with the packet to send, or ``None``. ``done`` is set once the last block
    is acknowledged, ``failed`` when the client ended the transfer or the
    resends ran out.
    """

    def __init__(self, source: BinaryIO, size: int, request: Request, client: Address,
                 retries: int = RETRIES) -> None:
        self.source = source
        self.client = client
        self.retries = retries
        self.blksize = DEFAULT_BLKSIZE
        self.acknowledged = []  # type: List[Tuple[str, str]]
        for key, value in request.options.items():
            if key == "blksize" and value.isdigit():
                self.blksize = max(MIN_BLKSIZE, min(int(value), MAX_BLKSIZE))
                self.acknowledged.append(("blksize", str(self.blksize)))
            elif key == "tsize":
                self.acknowledged.append(("tsize", str(size)))
        self.block = 0  # the block number the client must acknowledge next, counting past 65535
        self.waiting_for_option_ack = bool(self.acknowledged)
        self.last_data = False
        self.done = False
        self.failed = None  # type: Optional[str]
        self.resends = 0
        self.packet = b""

    def first(self) -> bytes:
        """The option acknowledgement, or the first data block."""
        if self.acknowledged:
            body = b"".join(k.encode("ascii") + b"\0" + v.encode("ascii") + b"\0"
                            for k, v in self.acknowledged)
            self.packet = struct.pack(">H", OP_OACK) + body
        else:
            self.block = 1
            self.packet = self._data()
        return self.packet

    def _data(self) -> bytes:
        chunk = self.source.read(self.blksize)
        self.last_data = len(chunk) < self.blksize
        return struct.pack(">HH", OP_DATA, self.block & 0xFFFF) + chunk

    def on_packet(self, data: bytes, address: Address) -> Optional[bytes]:
        """Handle a packet from ``address``; return the packet to send, if any."""
        if self.done or self.failed or address != self.client or len(data) < 4:
            return None
        opcode, number = struct.unpack(">HH", data[:4])
        if opcode == OP_ERROR:
            self.failed = "the client ended the transfer"
            return None
        if opcode != OP_ACK or number != self.block & 0xFFFF:
            return None  # an old or foreign acknowledgement: the resend timer deals with loss
        self.resends = 0
        if self.last_data and not self.waiting_for_option_ack:
            self.done = True
            return None
        self.waiting_for_option_ack = False
        self.block += 1
        self.packet = self._data()
        return self.packet

    def on_timeout(self) -> Optional[bytes]:
        """No answer for ``TIMEOUT`` seconds: resend, or give up after ``retries`` resends."""
        if self.done or self.failed:
            return None
        if self.resends >= self.retries:
            self.failed = "no answer from the client"
            return None
        self.resends += 1
        return self.packet


def _stderr_log(message: str) -> None:
    sys.stderr.write("%s %s\n" % (time.strftime("%Y-%m-%dT%H:%M:%S"), message))
    sys.stderr.flush()


class Server:
    """Serves one directory to TFTP read requests, one transfer at a time."""

    def __init__(self, directory: str, bind: str, port: int = 69, *, timeout: float = TIMEOUT,
                 retries: int = RETRIES, log: Callable[[str], None] = _stderr_log) -> None:
        self.directory = directory
        self.bind = bind
        self.port = port
        self.timeout = timeout
        self.retries = retries
        self.log = log
        self.socket = None  # type: Optional[socket.socket]

    def open(self) -> int:
        """Bind the listening socket; return the port (useful with port 0)."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((self.bind, self.port))
        except BaseException:
            sock.close()
            raise
        self.socket = sock
        self.port = sock.getsockname()[1]
        return self.port

    def close(self) -> None:
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def serve(self, stop: Callable[[], bool]) -> None:
        """Answer requests until ``stop()`` is true."""
        assert self.socket is not None, "open() first"
        while not stop():
            ready, _w, _x = select.select([self.socket], [], [], 0.2)
            if not ready:
                continue
            packet, address = self.socket.recvfrom(2048)
            self.handle(packet, address)

    def _fresh_socket(self) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.bind, 0))
        sock.settimeout(self.timeout)
        return sock

    def _refuse(self, address: Address, error: ProtocolError, what: str) -> None:
        self.log("%s:%d %s refused: %s (%d)" % (address[0], address[1], what, error.text,
                                                 error.code))
        sock = self._fresh_socket()
        try:
            sock.sendto(error_packet(error.code, error.text), address)
        finally:
            sock.close()

    def handle(self, packet: bytes, address: Address) -> None:
        """Handle one request packet from the listening port, whole transfer included."""
        try:
            request = parse_request(packet)
            source, size = open_regular(self.directory, request.filename, request.mode)
        except ProtocolError as error:
            self._refuse(address, error, "request")
            return
        transfer = Transfer(source, size, request, address, self.retries)
        sock = self._fresh_socket()
        started = time.time()
        self.log("%s:%d read %s (%d bytes, block %d)"
                 % (address[0], address[1], request.filename, size, transfer.blksize))
        try:
            sock.sendto(transfer.first(), address)
            while not (transfer.done or transfer.failed):
                try:
                    data, peer = sock.recvfrom(65535)
                    answer = transfer.on_packet(data, peer)
                except socket.timeout:
                    answer = transfer.on_timeout()
                if answer is not None:
                    sock.sendto(answer, address)
        finally:
            sock.close()
            source.close()
        if transfer.done:
            self.log("%s:%d %s sent in %.1f s" % (address[0], address[1], request.filename,
                                                  time.time() - started))
        else:
            self.log("%s:%d %s stopped: %s" % (address[0], address[1], request.filename,
                                                transfer.failed))


def main(argv: Optional[List[str]] = None, stop: Optional[Callable[[], bool]] = None) -> int:
    """Run the server: ``python3 tftp.py --dir DIR --bind ADDRESS [--port 69]``."""
    parser = argparse.ArgumentParser(
        description="Read-only TFTP server for one directory, on one address.")
    parser.add_argument("--dir", required=True, help="the directory to serve")
    parser.add_argument("--bind", required=True, help="the one address to listen on")
    parser.add_argument("--port", type=int, default=69)
    parser.add_argument("--pidfile", help="write the process id here once listening")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.dir):
        sys.stderr.write("tftp: %s is not a directory\n" % args.dir)
        return 2
    server = Server(args.dir, args.bind, args.port)
    try:
        server.open()
    except OSError as exc:
        sys.stderr.write("tftp: cannot listen on %s:%d: %s\n" % (args.bind, args.port, exc))
        return 1
    stopping = []  # type: List[bool]
    if stop is None:
        for number in (signal.SIGTERM, signal.SIGINT):
            signal.signal(number, lambda *_: stopping.append(True))
        stop = lambda: bool(stopping)  # noqa: E731
    if args.pidfile:
        with open(args.pidfile, "w") as handle:
            handle.write("%d\n" % os.getpid())
    server.log("serving %s on %s:%d" % (args.dir, args.bind, server.port))
    try:
        server.serve(stop)
    finally:
        server.close()
        if args.pidfile:
            try:
                os.unlink(args.pidfile)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
