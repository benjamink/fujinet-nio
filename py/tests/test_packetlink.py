import unittest

from fujinet_tools import packetlink as pl
from fujinet_tools.fujibus import FujiBusSession, build_fuji_packet_decoded

# The same records are checked in tests/test_packet_link.cpp.
SYNC_GEN7 = bytes.fromhex("f54e10070100018702d04d")
SYNC_ACK_GEN7_4096 = bytes.fromhex("f54e110703000100103f78151a")
MINIMUM_GEN7 = bytes.fromhex("f54e01070600010206000900560373eb")
MINIMUM = bytes([0x01, 0x02, 0x06, 0x00, 0x09, 0x00])


class FakePort:
    """Answers each record the way docs/packet_link.md says NIO does.

    `answer` is the response body, None for no answer, "echo" to return the
    request, or an int to answer with that Error code."""

    def __init__(self, answer=b"\xAA", capacity=4096):
        self.answer = answer
        self.capacity = capacity
        self.rx = bytearray()
        self.out = bytearray()
        self.gen = None
        self.packets = 0
        self.last_request = None
        self.fujinet_link = True

    def write(self, data):
        self.rx.extend(data)
        rec = pl.extract(self.rx)
        if rec.kind == pl.KIND_SYNC:
            if not rec.body or rec.body[0] == 0:
                self.gen = None
                self.out += pl.encode(pl.KIND_ERROR, rec.generation, bytes([pl.ERR_UNSUPPORTED_VERSION]))
                return
            self.gen = rec.generation
            ack = bytes([min(rec.body[0], pl.VERSION), self.capacity & 0xFF, self.capacity >> 8])
            self.out += pl.encode(pl.KIND_SYNC_ACK, rec.generation, ack)
            return
        self.packets += 1
        self.last_request = rec.body
        if rec.generation != self.gen:
            self.out += pl.encode(pl.KIND_ERROR, rec.generation, b"\x04")
        elif isinstance(self.answer, int):
            self.out += pl.encode(pl.KIND_ERROR, rec.generation, bytes([self.answer]))
        elif self.answer == "echo":
            self.out += pl.encode(pl.KIND_PACKET, rec.generation, rec.body)
        elif self.answer is not None:
            self.out += pl.encode(pl.KIND_PACKET, rec.generation, self.answer)

    def flush(self):
        pass

    def read(self, n):
        data = bytes(self.out[:n])
        del self.out[:n]
        return data


class QuietSync(unittest.TestCase):
    """Skips the controller's pre-Sync pause; the fake peer needs none."""

    def setUp(self):
        self._quiet = pl.RESYNC_QUIET_S
        pl.RESYNC_QUIET_S = 0

    def tearDown(self):
        pl.RESYNC_QUIET_S = self._quiet


class RecordTests(unittest.TestCase):
    def test_known_records(self):
        self.assertEqual(pl.encode(pl.KIND_SYNC, 7, bytes([pl.VERSION])), SYNC_GEN7)
        self.assertEqual(pl.encode(pl.KIND_SYNC_ACK, 7, bytes([1, 0x00, 0x10])), SYNC_ACK_GEN7_4096)
        self.assertEqual(pl.encode(pl.KIND_PACKET, 7, MINIMUM), MINIMUM_GEN7)

    def test_noise_and_corruption_are_skipped(self):
        bad = bytearray(pl.encode(pl.KIND_PACKET, 1, b"\x01\x02"))
        bad[7] ^= 0xFF
        rx = bytearray(b"\x00\xF5\x12") + bad + MINIMUM_GEN7
        rec = pl.extract(rx)
        self.assertEqual((rec.kind, rec.generation, rec.body), (pl.KIND_PACKET, 7, MINIMUM))
        self.assertEqual(rx, bytearray())

    def test_incomplete_record_waits(self):
        rx = bytearray(MINIMUM_GEN7[:-1])
        self.assertIsNone(pl.extract(rx))
        rx += MINIMUM_GEN7[-1:]
        self.assertEqual(pl.extract(rx).body, MINIMUM)


class ControllerTests(QuietSync):
    def test_exchange_after_sync(self):
        port = FakePort()
        link = pl.Controller(port)
        link.sync()
        self.assertEqual((link.version, link.capacity), (1, 4096))
        self.assertEqual(link.exchange(MINIMUM, timeout=1), b"\xAA")

    def test_request_over_capacity_is_refused_without_sending(self):
        port = FakePort(capacity=8)
        link = pl.Controller(port)
        link.sync()
        with self.assertRaises(pl.LinkError):
            link.exchange(bytes(9), timeout=1)
        self.assertEqual(port.packets, 0)

    def test_unanswered_is_an_unknown_completion(self):
        port = FakePort(answer=pl.ERR_UNANSWERED)
        link = pl.Controller(port)
        link.sync()
        before = link.generation
        self.assertIsNone(link.exchange(MINIMUM, timeout=1))
        self.assertEqual(link.generation, (before + 1) & 0xFF)
        self.assertEqual(port.packets, 1)  # not resent

    def test_sync_refused_raises(self):
        port = FakePort()
        link = pl.Controller(port)
        real_write = port.write
        port.write = lambda data: real_write(pl.encode(pl.KIND_SYNC, data[3], b""))
        with self.assertRaises(pl.LinkError):
            link.sync()

    def test_error_completion_is_raised(self):
        port = FakePort()
        link = pl.Controller(port)
        link.sync()
        port.gen = None
        with self.assertRaises(pl.LinkError):
            link.exchange(MINIMUM, timeout=1)

    def test_timeout_starts_a_new_generation_without_resending(self):
        port = FakePort(answer=None)
        link = pl.Controller(port)
        link.sync()
        before = link.generation
        self.assertIsNone(link.exchange(MINIMUM, timeout=0.05))
        self.assertEqual(link.generation, (before + 1) & 0xFF)


class SessionTests(QuietSync):
    def test_send_command_expect_over_a_link(self):
        port = FakePort(answer="echo")
        session = FujiBusSession().attach(port)
        pkt = session.send_command_expect(
            device=0x70, command=0x42, payload=b"\x01\x02",
            expect_device=0x70, expect_command=0x42, timeout=1,
        )
        self.assertIsNotNone(pkt)
        self.assertEqual((pkt.device, pkt.command), (0x70, 0x42))

    def test_unknown_completion_returns_promptly(self):
        port = FakePort(answer=None)
        session = FujiBusSession().attach(port)
        pkt = session.send_command_expect(
            device=0x70, command=0x42, payload=b"",
            expect_device=0x70, expect_command=0x42, timeout=0.05,
        )
        self.assertIsNone(pkt)

    def test_request_is_the_raw_packet(self):
        port = FakePort(answer="echo")
        session = FujiBusSession().attach(port)
        session.send_command(0x70, 0x42, b"\xAB", timeout=1)
        # The link body is the raw FujiBus packet: no SLIP.
        self.assertEqual(port.last_request, build_fuji_packet_decoded(0x70, 0x42, b"\xAB"))
        self.assertIsNotNone(session.pop_matching(0x70, 0x42))


if __name__ == "__main__":
    unittest.main()
