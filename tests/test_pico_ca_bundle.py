"""The trusted-root bundle generator: what it keeps, what it drops, and the bytes it writes."""
import datetime
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("make_ca_bundle", ROOT / "pico2w/tools/make_ca_bundle.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
SAMPLE = ROOT / "tests/pico-ca/fixtures/roots-sample.pem"
FULL = ROOT / "pico2w/certs/mozilla-roots.pem"
NOW = datetime.datetime(2026, 11, 16, tzinfo=datetime.timezone.utc)


def utc(year):
    return datetime.datetime(year, 1, 1, tzinfo=datetime.timezone.utc)


def entries(blob):
    magic, = struct.unpack_from("8s", blob, 0)
    count, data_offset = struct.unpack_from("<II", blob, 8)
    rows = [struct.unpack_from("<IIHHHH", blob, 16 + 16 * i) for i in range(count)]
    return magic, count, data_offset, rows


class Generator(unittest.TestCase):
    def sample(self, now=NOW):
        return GEN.build(GEN.read_pem(SAMPLE.read_text()), now)

    def test_fnv1a_matches_the_published_vectors(self):
        self.assertEqual(GEN.fnv1a32(b""), 0x811C9DC5)
        self.assertEqual(GEN.fnv1a32(b"a"), 0xE40C292C)
        self.assertEqual(GEN.fnv1a32(b"foobar"), 0xBF9CF968)

    def test_keeps_the_usable_roots_and_names_the_reason_for_each_drop(self):
        blob, kept, dropped = self.sample()
        self.assertEqual(len(kept), 5)
        reasons = dict(dropped)
        self.assertEqual(len(reasons), 2)
        self.assertIn("1.2.840.113549.1.1.5", reasons["Certum Trusted Network CA"])  # sha1WithRSA
        self.assertIn("EC curve 1.3.132.0.35", reasons["e-Szigno TLS Root CA 2023"])  # secp521r1
        names = sorted(r.name() for r in kept)
        self.assertEqual(names, ["Amazon Root CA 3", "DigiCert Global Root G2", "ISRG Root X1",
                                 "ISRG Root X2", "SSL.com TLS ECC Root CA 2022"])

    def test_key_sizes_and_curves_are_read_correctly(self):
        _, kept, _ = self.sample()
        by_name = {r.name(): r for r in kept}
        self.assertEqual(by_name["ISRG Root X1"].rsa_bits, 4096)
        self.assertEqual(by_name["DigiCert Global Root G2"].rsa_bits, 2048)
        self.assertEqual(by_name["Amazon Root CA 3"].curve_oid, GEN.OID_SECP256R1)
        self.assertEqual(by_name["ISRG Root X2"].curve_oid, GEN.OID_SECP384R1)

    def test_expired_roots_are_dropped_at_the_snapshot_date(self):
        _, kept, dropped = self.sample(utc(2037))  # ISRG Root X1 expires in 2035
        self.assertNotIn("ISRG Root X1", [r.name() for r in kept])
        self.assertTrue(any(n == "ISRG Root X1" and r.startswith("expired") for n, r in dropped))
        _, kept_before, _ = self.sample(utc(2020))
        self.assertIn("ISRG Root X1", [r.name() for r in kept_before])

    def test_a_root_not_yet_valid_is_dropped(self):
        _, kept, dropped = self.sample(utc(2010))  # before ISRG Root X1 existed
        self.assertEqual(kept, [])
        self.assertTrue(all("not yet valid" in r for _, r in dropped if "Certum" not in _ and "Szigno" not in _))

    def test_layout_is_self_consistent(self):
        blob, kept, _ = self.sample()
        magic, count, data_offset, rows = entries(blob)
        self.assertEqual(magic, b"NIOCAB01")
        self.assertEqual(count, len(kept))
        self.assertEqual(data_offset, 16 + 16 * count)
        self.assertEqual(rows, sorted(rows, key=lambda r: (r[0], blob[data_offset + r[1]:data_offset + r[1] + r[2]])))
        end = 0
        for subject_hash, der_offset, der_len, subject_off, subject_len, reserved in rows:
            self.assertEqual(reserved, 0)
            der = blob[data_offset + der_offset:data_offset + der_offset + der_len]
            subject = der[subject_off:subject_off + subject_len]
            self.assertEqual(subject_hash, GEN.fnv1a32(subject))
            # The subject range is exactly one DER SEQUENCE (a Name), found without the generator's parser.
            tag, start, finish = GEN.tlv(der, subject_off)
            self.assertEqual(tag, 0x30)
            self.assertEqual(finish, subject_off + subject_len)
            self.assertEqual(der_offset, end)  # certificates are packed back to back, in index order
            end = der_offset + der_len
        self.assertEqual(len(blob), data_offset + end)

    def test_output_is_deterministic(self):
        self.assertEqual(self.sample()[0], self.sample()[0])
        pem = GEN.read_pem(SAMPLE.read_text())
        self.assertEqual(GEN.build(pem, NOW)[0], GEN.build(list(reversed(pem)), NOW)[0])  # input order does not matter

    def test_duplicates_are_kept_once(self):
        pem = GEN.read_pem(SAMPLE.read_text())
        blob, kept, dropped = GEN.build(pem + pem, NOW)
        self.assertEqual(len(kept), 5)
        self.assertTrue(any(reason == "duplicate" for _, reason in dropped))

    def test_garbage_is_dropped_not_trusted(self):
        blob, kept, dropped = GEN.build([b"\x30\x03\x02\x01\x01", b"not a certificate"], NOW)
        self.assertEqual(kept, [])
        self.assertEqual(len(dropped), 2)

    def test_header_file_carries_the_exact_bytes(self):
        blob, kept, _ = self.sample()
        text = GEN.c_header(blob, "test")
        body = text.split("ca_bundle[] = {", 1)[1].split("};", 1)[0]
        values = [int(x, 16) for x in body.replace("\n", " ").replace(",", " ").split()]
        self.assertEqual(bytes(values), blob)
        self.assertIn("ca_bundle_size", text)

    def test_command_line_writes_both_outputs_and_rejects_empty_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary, header = Path(tmp, "b.bin"), Path(tmp, "b.h")
            self.assertEqual(GEN.main([str(SAMPLE), "--bin", str(binary), "--header", str(header), "--now", "2026-11-16"]), 0)
            self.assertEqual(binary.read_bytes(), self.sample()[0])
            self.assertTrue(header.read_text().startswith("// Generated by"))
            empty = Path(tmp, "empty.pem")
            empty.write_text("nothing here\n")
            self.assertEqual(GEN.main([str(empty), "--bin", str(binary)]), 1)

    @unittest.skipUnless(FULL.exists(), "full Mozilla set not present")
    def test_the_full_mozilla_set_packs_and_every_kept_root_is_a_valid_entry(self):
        blob, kept, dropped = GEN.build(GEN.read_pem(FULL.read_text()), datetime.datetime(2026, 11, 5, tzinfo=datetime.timezone.utc))
        self.assertGreater(len(kept), 100)          # a trust store that shrank to a handful is a bug
        self.assertLess(len(dropped), 10)
        self.assertLess(len(blob), 160 * 1024)      # flash budget
        names = [r.name() for r in kept]
        for must_have in ("ISRG Root X1", "ISRG Root X2", "DigiCert Global Root G2", "GTS Root R1"):
            self.assertIn(must_have, names)


if __name__ == "__main__":
    unittest.main()
