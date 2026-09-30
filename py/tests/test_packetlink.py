import unittest

from fujinet_tools import packetlink as pl

# The same records are checked in tests/test_packet_link.cpp.
SYNC_GEN7 = bytes.fromhex("f54e10070000069e1274")
MINIMUM_GEN7 = bytes.fromhex("f54e01070600010206000900560373eb")
MINIMUM = bytes([0x01, 0x02, 0x06, 0x00, 0x09, 0x00])


class FakePort:
    """Answers each record the way docs/packet_link.md says NIO does."""

    def __init__(self, answer=b"\xAA"):
        self.answer = answer
        self.rx = bytearray()
        self.out = bytearray()
        self.gen = None

    def write(self, data):
        self.rx.extend(data)
        rec = pl.extract(self.rx)
        if rec.kind == pl.KIND_SYNC:
            self.gen = rec.generation
            self.out += pl.encode(pl.KIND_SYNC_ACK, rec.generation)
        elif rec.generation != self.gen:
            self.out += pl.encode(pl.KIND_ERROR, rec.generation, b"\x04")
        elif self.answer is not None:
            self.out += pl.encode(pl.KIND_PACKET, rec.generation, self.answer)

    def flush(self):
        pass

    def read(self, n):
        data = bytes(self.out[:n])
        del self.out[:n]
        return data


class RecordTests(unittest.TestCase):
    def test_known_records(self):
        self.assertEqual(pl.encode(pl.KIND_SYNC, 7), SYNC_GEN7)
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


class ControllerTests(unittest.TestCase):
    def test_exchange_after_sync(self):
        port = FakePort()
        link = pl.Controller(port)
        link.sync()
        self.assertEqual(link.exchange(MINIMUM, timeout=1), b"\xAA")

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


if __name__ == "__main__":
    unittest.main()
