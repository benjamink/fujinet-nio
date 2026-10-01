# py/fujinet_tools/packetlink.py
"""
Controller side of the packet link (docs/packet_link.md): raw FujiBus packets
in CRC-checked records, one request at a time, within a Sync'd generation.
"""

from __future__ import annotations

import time
import zlib
from dataclasses import dataclass
from typing import Optional

SYNC = b"\xF5\x4E"
HEADER = 6
CRC = 4

VERSION = 1
# The peer abandons a half-received record after 100 ms without bytes; a
# controller stays quiet a little longer than that before every Sync.
RESYNC_QUIET_S = 0.12

KIND_PACKET = 0x01
KIND_ERROR = 0x02
KIND_SYNC = 0x10
KIND_SYNC_ACK = 0x11

ERR_UNANSWERED = 6
ERR_UNSUPPORTED_VERSION = 7

# Code 3 is reserved (formerly Busy).
ERRORS = {
    1: "Corrupt",
    2: "Oversized",
    4: "NotSynchronised",
    5: "Empty",
    ERR_UNANSWERED: "Unanswered",
    ERR_UNSUPPORTED_VERSION: "UnsupportedVersion",
}


@dataclass
class Record:
    kind: int
    generation: int
    body: bytes


def encode(kind: int, generation: int, body: bytes = b"") -> bytes:
    head = bytes([kind, generation & 0xFF, len(body) & 0xFF, len(body) >> 8])
    crc = zlib.crc32(head + body) & 0xFFFFFFFF
    return SYNC + head + body + crc.to_bytes(4, "little")


def extract(rx: bytearray, capacity: int = 65535) -> Optional[Record]:
    """Take the first valid record from `rx`, dropping noise; None if incomplete."""
    while True:
        at = rx.find(SYNC)
        if at < 0:
            del rx[: max(0, len(rx) - 1)]
            return None
        del rx[:at]
        if len(rx) < HEADER:
            return None
        size = rx[4] | (rx[5] << 8)
        if size > capacity:
            del rx[:2]
            continue
        if len(rx) < HEADER + size + CRC:
            return None
        crc = int.from_bytes(rx[HEADER + size : HEADER + size + CRC], "little")
        if crc != zlib.crc32(bytes(rx[2 : HEADER + size])) & 0xFFFFFFFF:
            del rx[:2]
            continue
        rec = Record(rx[2], rx[3], bytes(rx[HEADER : HEADER + size]))
        del rx[: HEADER + size + CRC]
        return rec


class LinkError(Exception):
    pass


class Controller:
    """Sends requests over a pyserial-like port and returns the peer's answers."""

    def __init__(self, port, *, generation: int = 1):
        self._port = port
        self._rx = bytearray()
        self.generation = generation & 0xFF
        self.version: Optional[int] = None  # agreed in SyncAck
        self.capacity: Optional[int] = None  # the peer's raw-packet capacity

    def _read_record(self, deadline: float) -> Optional[Record]:
        while time.monotonic() < deadline:
            rec = extract(self._rx)
            if rec is not None:
                return rec
            chunk = self._port.read(max(1, getattr(self._port, "in_waiting", 0) or 0))
            if chunk:
                self._rx.extend(chunk)
        return None

    def sync(self, timeout: float = 2.0) -> None:
        self.generation = (self.generation + 1) & 0xFF
        self.version = self.capacity = None
        # Quiet first, so the peer abandons anything half received.
        time.sleep(RESYNC_QUIET_S)
        self._rx.clear()
        self._port.write(encode(KIND_SYNC, self.generation, bytes([VERSION])))
        self._port.flush()
        deadline = time.monotonic() + timeout
        while True:
            rec = self._read_record(deadline)
            if rec is None:
                raise LinkError("no SyncAck from the peer")
            if rec.generation != self.generation:
                continue
            if rec.kind == KIND_ERROR:
                code = rec.body[0] if rec.body else 0
                raise LinkError(f"peer refused Sync: {ERRORS.get(code, code)}")
            if rec.kind == KIND_SYNC_ACK:
                if len(rec.body) < 3 or rec.body[0] == 0 or rec.body[0] > VERSION:
                    raise LinkError(f"unusable SyncAck {rec.body.hex()}")
                self.version = rec.body[0]
                self.capacity = rec.body[1] | (rec.body[2] << 8)
                return

    def exchange(self, packet: bytes, timeout: float) -> Optional[bytes]:
        """One request; the response packet, or None after an unknown completion
        (a timeout or Unanswered), after which the link is resynced. Raises
        LinkError when the request was definitely not executed."""
        if self.capacity is None:
            raise LinkError("not synchronised")
        if len(packet) > self.capacity:
            raise LinkError(f"request of {len(packet)} bytes exceeds the peer's capacity of {self.capacity}")
        self._port.write(encode(KIND_PACKET, self.generation, packet))
        self._port.flush()
        deadline = time.monotonic() + timeout
        while True:
            rec = self._read_record(deadline)
            if rec is None:
                # Unknown completion: start a new generation, never resend.
                self.sync()
                return None
            if rec.generation != self.generation:
                continue
            if rec.kind == KIND_PACKET:
                return rec.body
            if rec.kind == KIND_ERROR:
                code = rec.body[0] if rec.body else 0
                if code == ERR_UNANSWERED:
                    # NIO took it but gave no answer; it may have run.
                    self.sync()
                    return None
                raise LinkError(f"peer rejected the request: {ERRORS.get(code, code)}")
