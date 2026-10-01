import unittest

from fujinet_tools import packetlink as pl
from fujinet_tools.fujibus import FujiBusSession, build_fuji_packet_decoded

# The same records are checked in tests/test_packet_link.cpp.
SYNC_GEN7 = bytes.fromhex("f54e10070100018702d04d")
SYNC_ACK_GEN7_4096_100MS = bytes.fromhex("f54e11070500010010640041409825")
MINIMUM_GEN7 = bytes.fromhex("f54e01070600010206000900560373eb")
MINIMUM = bytes([0x01, 0x02, 0x06, 0x00, 0x09, 0x00])


class FakePort:
    """Answers each record the way docs/packet_link.md says NIO does.

    `answer` is the response body, None for no answer, "echo" to return the
    request, or an int to answer with that Error code."""

    def __init__(self, answer=b"\xAA", capacity=4096, record_timeout_ms=100, ignore_syncs=0):
        self.answer = answer
        self.capacity = capacity
        self.record_timeout_ms = record_timeout_ms
        self.ignore_syncs = ignore_syncs  # Syncs to swallow, as a cut-off record would
        self.syncs_seen = 0
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
            self.syncs_seen += 1
            if self.syncs_seen <= self.ignore_syncs:
                return
            if not rec.body or rec.body[0] == 0:
                self.gen = None
                self.out += pl.encode(pl.KIND_ERROR, rec.generation, bytes([pl.ERR_UNSUPPORTED_VERSION]))
                return
            self.gen = rec.generation
            t = self.record_timeout_ms
            ack = bytes([min(rec.body[0], pl.VERSION), self.capacity & 0xFF, self.capacity >> 8, t & 0xFF, t >> 8])
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
    """Records the controller's pre-Sync pauses instead of sleeping them."""

    def setUp(self):
        self.sleeps = []
        self._sleep = pl.time.sleep
        pl.time.sleep = self.sleeps.append

    def tearDown(self):
        pl.time.sleep = self._sleep


class RecordTests(unittest.TestCase):
    def test_known_records(self):
        self.assertEqual(pl.encode(pl.KIND_SYNC, 7, bytes([pl.VERSION])), SYNC_GEN7)
        self.assertEqual(pl.encode(pl.KIND_SYNC_ACK, 7, bytes([1, 0x00, 0x10, 100, 0])), SYNC_ACK_GEN7_4096_100MS)
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
        self.assertEqual((link.version, link.capacity, link.record_timeout_ms), (1, 4096, 100))
        self.assertEqual(link.exchange(MINIMUM, timeout=1), b"\xAA")

    def test_quiet_before_sync_follows_the_advertised_timeout(self):
        port = FakePort(record_timeout_ms=2500)
        link = pl.Controller(port)
        link.sync()
        link.sync()
        # Before the first SyncAck: the default; afterwards: the peer's value.
        self.assertAlmostEqual(self.sleeps[0], 0.1 * pl.QUIET_MARGIN)
        self.assertAlmostEqual(self.sleeps[1], 2.5 * pl.QUIET_MARGIN)

    def test_unanswered_sync_doubles_the_quiet_period(self):
        port = FakePort(ignore_syncs=2)
        link = pl.Controller(port)
        real_attempt = pl.SYNC_ATTEMPT_S
        pl.SYNC_ATTEMPT_S = 0.01
        try:
            link.sync()
        finally:
            pl.SYNC_ATTEMPT_S = real_attempt
        base = 0.1 * pl.QUIET_MARGIN
        self.assertEqual(len(self.sleeps), 3)
        for got, want in zip(self.sleeps, [base, 2 * base, 4 * base]):
            self.assertAlmostEqual(got, want)
        self.assertEqual(link.capacity, 4096)

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
