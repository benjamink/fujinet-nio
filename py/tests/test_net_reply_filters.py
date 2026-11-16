"""The reply filters of `fujinet net send/get/read`.

A NotReady, DeviceBusy or error reply has no payload, so a filter that insists on matching a
handle and offset throws it away. The retry loop then never sees the status it must act on and
the command fails after the whole timeout with "No response". `net send --method DELETE` did
exactly that: the first Read came back NotReady (the server had not answered yet) and was
discarded, though the response was ready a few milliseconds later.
"""
from __future__ import annotations

import struct
import unittest

from fujinet_tools import net
from fujinet_tools import netproto as np
from fujinet_tools.fujibus import FujiPacket

OK, DEVICE_BUSY, NOT_READY, IO_ERROR = 0, 3, 4, 5


def packet(status: int, payload: bytes = b"") -> FujiPacket:
    return FujiPacket(device=np.NETWORK_DEVICE_ID, command=np.CMD_READ, length=0, checksum=0,
                      checksum_computed=0, checksum_ok=True, descr=0, params=[status], payload=payload)


def read_reply(handle: int, offset: int, data: bytes = b"x") -> bytes:
    # version, flags, available, handle, offset, data_len, data
    return struct.pack("<BBHHIH", np.NETPROTO_VERSION, 0, 0, handle, offset, len(data)) + data


def write_reply(handle: int, offset: int, written: int = 1024) -> bytes:
    # version, flags, reserved, handle, offset, written
    return struct.pack("<BBHHIH", np.NETPROTO_VERSION, 0, 0, handle, offset, written)


class ReadFilters(unittest.TestCase):
    def test_a_not_ready_reply_is_accepted_so_the_retry_loop_sees_it(self) -> None:
        for accept in (net._accept_read_at(0), net._accept_read_for(7, 0)):
            self.assertTrue(accept(packet(NOT_READY)))
            self.assertTrue(accept(packet(DEVICE_BUSY)))

    def test_an_error_reply_is_accepted_so_the_caller_can_report_it(self) -> None:
        self.assertTrue(net._accept_read_at(0)(packet(IO_ERROR)))
        self.assertTrue(net._accept_read_for(7, 0)(packet(IO_ERROR)))

    def test_an_ok_reply_must_match_the_offset(self) -> None:
        accept = net._accept_read_at(4096)
        self.assertTrue(accept(packet(OK, read_reply(7, 4096))))
        self.assertFalse(accept(packet(OK, read_reply(7, 0))))   # an earlier chunk's reply

    def test_an_ok_reply_must_match_handle_and_offset(self) -> None:
        accept = net._accept_read_for(7, 1024)
        self.assertTrue(accept(packet(OK, read_reply(7, 1024))))
        self.assertFalse(accept(packet(OK, read_reply(8, 1024))))   # another session's reply
        self.assertFalse(accept(packet(OK, read_reply(7, 0))))

    def test_an_ok_reply_that_does_not_parse_is_not_accepted(self) -> None:
        self.assertFalse(net._accept_read_for(7, 0)(packet(OK, b"\x01\x02")))
        self.assertFalse(net._accept_read_at(0)(packet(OK, b"")))


class WriteFilters(unittest.TestCase):
    def test_a_busy_or_not_ready_write_is_accepted_for_retry(self) -> None:
        accept = net._accept_write_for(7, 7168)
        self.assertTrue(accept(packet(DEVICE_BUSY)))   # the TCP send buffer is full: ask again
        self.assertTrue(accept(packet(NOT_READY)))
        self.assertTrue(accept(packet(IO_ERROR)))

    def test_an_ok_write_must_match_handle_and_offset(self) -> None:
        accept = net._accept_write_for(7, 1024)
        self.assertTrue(accept(packet(OK, write_reply(7, 1024))))
        self.assertFalse(accept(packet(OK, write_reply(7, 0))))
        self.assertFalse(accept(packet(OK, write_reply(9, 1024))))
        self.assertFalse(accept(packet(OK, b"")))


class RetryLoopSeesTheStatus(unittest.TestCase):
    def test_not_ready_then_ok_is_retried_not_timed_out(self) -> None:
        """End to end through _send_retry_not_ready: the first reply is NotReady, the next is OK."""
        replies = [packet(NOT_READY), packet(OK, read_reply(7, 0, b"hello"))]
        sent = []

        class Bus:
            def send_command_expect(self, **kw):
                accept = kw["accept"]
                reply = replies[len(sent)]
                sent.append(kw["command"])
                # The real bus drops a reply the filter refuses and keeps waiting until the timeout.
                return reply if accept(reply) else None

        pkt = net._send_retry_not_ready(
            bus=Bus(), device=np.NETWORK_DEVICE_ID, command=np.CMD_READ, payload=b"", timeout=5.0,
            retries=5, sleep_s=0.001, accept=net._accept_read_for(7, 0),
        )
        self.assertIsNotNone(pkt)
        self.assertEqual(len(sent), 2)                       # asked twice
        self.assertEqual(np.parse_read_resp(pkt.payload).data, b"hello")


if __name__ == "__main__":
    unittest.main()
