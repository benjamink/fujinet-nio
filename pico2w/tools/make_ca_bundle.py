#!/usr/bin/env python3
"""Build the Pico 2 W trusted-root bundle from a PEM file of CA certificates.

The device does not parse the whole set: mbedTLS asks for "the roots whose subject is X"
while it verifies a chain, and the device parses only those. So the bundle is the roots'
DER certificates plus a small index, sorted by a hash of each subject name, that lets the
device find the candidates for an issuer without touching the others.

Layout (little endian):

    header   16 bytes   "NIOCAB01", u32 count, u32 data_offset
    index    16 bytes per root, sorted by (subject_hash, der_offset):
                 u32 subject_hash   FNV-1a 32 of the subject Name DER
                 u32 der_offset     of the certificate, from the start of the data area
                 u16 der_len
                 u16 subject_off    of the subject Name DER, from the start of the certificate
                 u16 subject_len
                 u16 reserved       0
    data     the certificates, DER, in index order

Roots the device's mbedTLS build cannot use are dropped (and reported), so every root in the
bundle can be parsed and used to verify a chain:

  * public key: RSA up to 4096 bits, or EC on secp256r1 / secp384r1 (the curves enabled in
    mbedtls_config.h; P-521 and Brainpool are not);
  * signature: RSA or ECDSA with SHA-256/384/512 (no SHA-1, MD5 or RSASSA-PSS);
  * not expired at --now (default: today).

The output is deterministic: the same PEM and --now give the same bytes.
"""

import argparse
import base64
import datetime
import hashlib
import re
import struct
import sys

MAGIC = b"NIOCAB01"
HEADER_LEN = 16
ENTRY_LEN = 16

OID_RSA = "1.2.840.113549.1.1.1"
OID_EC = "1.2.840.10045.2.1"
OID_SECP256R1 = "1.2.840.10045.3.1.7"
OID_SECP384R1 = "1.3.132.0.34"
SUPPORTED_CURVES = {OID_SECP256R1: "secp256r1", OID_SECP384R1: "secp384r1"}
SUPPORTED_SIG = {
    "1.2.840.113549.1.1.11": "sha256WithRSA",
    "1.2.840.113549.1.1.12": "sha384WithRSA",
    "1.2.840.113549.1.1.13": "sha512WithRSA",
    "1.2.840.10045.4.3.2": "ecdsa-with-SHA256",
    "1.2.840.10045.4.3.3": "ecdsa-with-SHA384",
    "1.2.840.10045.4.3.4": "ecdsa-with-SHA512",
}
MAX_RSA_BITS = 4096


class DerError(ValueError):
    pass


def tlv(buf, pos):
    """Read one DER element at pos: (tag, content_start, content_end)."""
    if pos + 2 > len(buf):
        raise DerError("truncated element")
    tag = buf[pos]
    length = buf[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7F
        if count == 0 or count > 4 or pos + count > len(buf):
            raise DerError("bad length")
        length = int.from_bytes(buf[pos:pos + count], "big")
        pos += count
    if pos + length > len(buf):
        raise DerError("element overruns its parent")
    return tag, pos, pos + length


def children(buf, start, end):
    pos = start
    while pos < end:
        tag, cs, ce = tlv(buf, pos)
        yield tag, pos, cs, ce
        pos = ce


def oid_text(buf, start, end):
    if start >= end:
        raise DerError("empty OID")
    parts = []
    first = buf[start]
    parts.extend([first // 40, first % 40])
    value = 0
    for byte in buf[start + 1:end]:
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            parts.append(value)
            value = 0
    return ".".join(str(p) for p in parts)


def parse_time(buf, tag, start, end):
    text = buf[start:end].decode("ascii")
    if tag == 0x17:  # UTCTime YYMMDDHHMMSSZ
        year = int(text[0:2])
        year += 2000 if year < 50 else 1900
        rest = text[2:]
    elif tag == 0x18:  # GeneralizedTime YYYYMMDDHHMMSSZ
        year = int(text[0:4])
        rest = text[4:]
    else:
        raise DerError("unexpected time tag")
    return datetime.datetime(year, int(rest[0:2]), int(rest[2:4]), int(rest[4:6]),
                             int(rest[6:8]), int(rest[8:10]), tzinfo=datetime.timezone.utc)


class Root:
    """The fields of a certificate the bundle needs."""

    def __init__(self, der):
        self.der = der
        top_tag, cs, ce = tlv(der, 0)
        if top_tag != 0x30 or ce != len(der):
            raise DerError("not a single certificate")
        items = list(children(der, cs, ce))
        if len(items) != 3:
            raise DerError("certificate is not tbs, algorithm, signature")
        tbs = items[0]
        outer_alg = items[1]
        fields = list(children(der, tbs[2], tbs[3]))
        i = 0
        if fields[i][0] == 0xA0:  # explicit version
            i += 1
        # serial, signature alg, issuer, validity, subject, spki
        if len(fields) < i + 6:
            raise DerError("tbsCertificate is too short")
        _serial, tbs_alg, _issuer, validity, subject, spki = fields[i:i + 6]
        self.subject_off = subject[1]
        self.subject_len = subject[3] - subject[1]
        self.subject = der[self.subject_off:subject[3]]

        alg_items = list(children(der, outer_alg[2], outer_alg[3]))
        self.sig_oid = oid_text(der, alg_items[0][2], alg_items[0][3])
        tbs_alg_items = list(children(der, tbs_alg[2], tbs_alg[3]))
        self.tbs_sig_oid = oid_text(der, tbs_alg_items[0][2], tbs_alg_items[0][3])

        times = list(children(der, validity[2], validity[3]))
        self.not_before = parse_time(der, times[0][0], times[0][2], times[0][3])
        self.not_after = parse_time(der, times[1][0], times[1][2], times[1][3])

        spki_items = list(children(der, spki[2], spki[3]))
        key_alg = list(children(der, spki_items[0][2], spki_items[0][3]))
        self.key_oid = oid_text(der, key_alg[0][2], key_alg[0][3])
        self.curve_oid = None
        self.rsa_bits = None
        if self.key_oid == OID_EC and len(key_alg) > 1 and key_alg[1][0] == 0x06:
            self.curve_oid = oid_text(der, key_alg[1][2], key_alg[1][3])
        elif self.key_oid == OID_RSA:
            bit_string = spki_items[1]
            # BIT STRING: first content byte is the unused-bit count, then RSAPublicKey.
            inner = bit_string[2] + 1
            rsa_tag, rs, re_ = tlv(der, inner)
            modulus = list(children(der, rs, re_))[0]
            value = der[modulus[2]:modulus[3]]
            value = value.lstrip(b"\x00")
            self.rsa_bits = len(value) * 8 - (8 - value[0].bit_length()) if value else 0

    def name(self):
        """A readable subject for reports: the CN or O, else a hash."""
        for oid_bytes, label in ((b"\x55\x04\x03", "CN"), (b"\x55\x04\x0a", "O")):
            at = self.subject.find(oid_bytes)
            if at >= 0 and at + 5 < len(self.subject):
                length = self.subject[at + 4]
                value = self.subject[at + 5:at + 5 + length]
                return value.decode("utf-8", "replace")
        return hashlib.sha256(self.subject).hexdigest()[:12]

    def drop_reason(self, now):
        if self.sig_oid not in SUPPORTED_SIG or self.tbs_sig_oid != self.sig_oid:
            return "signature algorithm " + self.sig_oid
        if self.key_oid == OID_EC:
            if self.curve_oid not in SUPPORTED_CURVES:
                return "EC curve " + str(self.curve_oid)
        elif self.key_oid == OID_RSA:
            if not self.rsa_bits or self.rsa_bits > MAX_RSA_BITS:
                return "RSA %s bits" % self.rsa_bits
        else:
            return "key algorithm " + self.key_oid
        if self.not_after < now:
            return "expired " + self.not_after.date().isoformat()
        if self.not_before > now:
            return "not yet valid " + self.not_before.date().isoformat()
        return None


def fnv1a32(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def read_pem(text):
    ders = []
    for block in re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S):
        ders.append(base64.b64decode("".join(block.split())))
    return ders


def build(ders, now):
    """Return (bundle_bytes, kept_roots, dropped) for the DER certificates."""
    kept = []
    dropped = []
    seen = set()
    for der in ders:
        try:
            root = Root(der)
        except (DerError, IndexError, ValueError) as error:
            dropped.append(("(unparsed)", "unparsable: %s" % error))
            continue
        reason = root.drop_reason(now)
        if reason:
            dropped.append((root.name(), reason))
        elif der in seen:
            dropped.append((root.name(), "duplicate"))
        else:
            seen.add(der)
            kept.append(root)

    entries = []
    offset = 0
    for root in kept:
        entries.append((fnv1a32(root.subject), root))
    entries.sort(key=lambda e: (e[0], e[1].der))

    index = bytearray()
    data = bytearray()
    for subject_hash, root in entries:
        if len(root.der) > 0xFFFF or root.subject_off > 0xFFFF or root.subject_len > 0xFFFF:
            raise DerError("certificate too large for the index: " + root.name())
        index += struct.pack("<IIHHHH", subject_hash, len(data), len(root.der),
                             root.subject_off, root.subject_len, 0)
        data += root.der
    data_offset = HEADER_LEN + len(index)
    header = MAGIC + struct.pack("<II", len(entries), data_offset)
    return bytes(header + index + data), [r for _, r in entries], dropped


def c_header(blob, source_note):
    lines = ["// Generated by pico2w/tools/make_ca_bundle.py. Do not edit.",
             "// " + source_note, "#pragma once", "#include <cstddef>", "#include <cstdint>",
             "namespace fujinet::platform::pico {",
             "inline constexpr std::uint8_t ca_bundle[] = {"]
    for at in range(0, len(blob), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in blob[at:at + 16]) + ",")
    lines += ["};", "inline constexpr std::size_t ca_bundle_size = sizeof(ca_bundle);",
              "}  // namespace fujinet::platform::pico", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pem", help="PEM file of CA certificates")
    parser.add_argument("--bin", help="write the binary bundle here")
    parser.add_argument("--header", help="write a C++ header with the bundle here")
    parser.add_argument("--now", help="date for the expiry check, YYYY-MM-DD (default: today)")
    parser.add_argument("--report", action="store_true", help="list kept and dropped roots")
    args = parser.parse_args(argv)

    if args.now:
        now = datetime.datetime.fromisoformat(args.now).replace(tzinfo=datetime.timezone.utc)
    else:
        now = datetime.datetime.now(datetime.timezone.utc)
    with open(args.pem, "r", encoding="ascii", errors="replace") as handle:
        ders = read_pem(handle.read())
    if not ders:
        print("error: no certificates found in " + args.pem, file=sys.stderr)
        return 1
    blob, kept, dropped = build(ders, now)
    if not kept:
        print("error: every certificate was dropped", file=sys.stderr)
        return 1
    if args.bin:
        with open(args.bin, "wb") as handle:
            handle.write(blob)
    if args.header:
        note = "%d roots from %s, %d bytes" % (len(kept), args.pem.split("/")[-1], len(blob))
        with open(args.header, "w", encoding="ascii") as handle:
            handle.write(c_header(blob, note))
    print("%d roots kept, %d dropped, %d bytes" % (len(kept), len(dropped), len(blob)))
    if args.report:
        for name, reason in dropped:
            print("  dropped: %s (%s)" % (name, reason))
    return 0


if __name__ == "__main__":
    sys.exit(main())
