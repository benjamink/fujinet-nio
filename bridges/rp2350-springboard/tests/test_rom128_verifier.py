"""Offline malformed-artifact tests; real SDK firmware is checked separately."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("verify_rom128", ROOT / "scripts/verify_rom128_firmware.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def block(payload, number=0, total=2, flags=0x2000, family=0xE48BFF59, address=None, tail=b""):
    address = 0x10000000 + number * 256 if address is None else address
    header = struct.pack("<8I", 0x0A324655, 0x9E5D5157, flags, address, 256, number, total, family)
    return header + payload.ljust(256, b"\0") + tail.ljust(220, b"\0") + struct.pack("<I", 0x0AB16F30)


def firmware(binary):
    count = (len(binary) + 255) // 256
    return b"".join(block(binary[i * 256:(i + 1) * 256], i, count) for i in range(count))


def compatibility():
    return block(b"\xef" * 256, flags=0xA000, family=0xE48BFF57,
                 address=0x10FFFF00, tail=struct.pack("<I", 0x9957E304))


def word(data, index, value):
    result = bytearray(data)
    struct.pack_into("<I", result, index * 4, value)
    return bytes(result)


class UF2Verifier(unittest.TestCase):
    def setUp(self):
        self.binary = bytes(range(256)) + b"last partial page"
        self.uf2 = firmware(self.binary)

    def test_valid_firmware_and_optional_compatibility_block(self):
        for data in (self.uf2, compatibility() + self.uf2,
                     compatibility() + self.uf2[512:] + self.uf2[:512]):
            with self.subTest(compatibility=len(data) > 1024):
                verifier.verify_uf2(data, self.binary)

    def test_bad_firmware_headers_and_data(self):
        changes = {
            "start magic": (0, 0), "second magic": (1, 0),
            "missing family flag": (2, 0), "non-flash flag": (2, 0x2001),
            "extension flag": (2, 0xA000), "unknown flag": (2, 0x12000),
            "address": (3, 0x10000004), "payload size": (4, 512),
            "block number": (5, 5), "block count": (6, 3),
            "family": (7, 0xE48BFF56), "end magic": (127, 0),
            "payload": (8, 0), "unexpected metadata": (72, 1),
        }
        for name, (index, value) in changes.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                verifier.verify_uf2(word(self.uf2, index, value), self.binary)

    def test_missing_duplicate_and_truncated_blocks(self):
        for data in (b"", self.uf2[:512], self.uf2 + self.uf2[:512], self.uf2[:-1]):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                verifier.verify_uf2(data, self.binary)
        with self.assertRaisesRegex(ValueError, "BIN is empty"):
            verifier.verify_uf2(self.uf2, b"")

    def test_bad_compatibility_blocks(self):
        changes = {
            "flags": (2, 0x2000), "address": (3, 0x10000000),
            "payload size": (4, 128), "block number": (5, 1), "block count": (6, 1),
            "family": (7, 0x12345678), "payload": (8, 0),
            "ignore tag": (72, 0x12345604), "extension terminator": (73, 1),
        }
        for name, (index, value) in changes.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                verifier.verify_uf2(word(compatibility(), index, value) + self.uf2, self.binary)
        for data in (compatibility() * 2 + self.uf2, self.uf2 + compatibility()):
            with self.assertRaises(ValueError):
                verifier.verify_uf2(data, self.binary)

    def test_extension_tag_cannot_hide_extra_firmware(self):
        # The former blanket 0x8000 skip accepted this unchecked extra block.
        extra = block(b"extra firmware", flags=0xA000, address=0x10008000,
                      tail=struct.pack("<I", 0x12345604))
        with self.assertRaisesRegex(ValueError, "firmware flags"):
            verifier.verify_uf2(self.uf2 + extra, self.binary)

    def test_final_page_padding_is_checked(self):
        damaged = bytearray(self.uf2)
        damaged[512 + 32 + len(self.binary) - 256] = 1
        with self.assertRaisesRegex(ValueError, "final-page padding"):
            verifier.verify_uf2(bytes(damaged), self.binary)


class ELFVerifier(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.rom = self.build / "full.rom"
        self.image = bytes(range(256)) * 512
        self.rom.write_bytes(self.image)
        address = 0x10001000
        self.symbols = {
            "sbnio_rom": address, "sbnio_rom_size": address + len(self.image),
            "bus_rom_words": address + len(self.image) + 4,
            "bus_cs1_words": address + len(self.image) + 8,
            "table_backing": 0x20020000, "__bss_end__": 0x20060000,
            "__HeapLimit": 0x20080000, "sb_persistent_reply": 0x20080000,
            "__scratch_x_end__": 0x20080200, "__StackOneBottom": 0x20080800,
            "__StackOneTop": 0x20081000, "__StackBottom": 0x20081000,
            "__StackTop": 0x20082000,
        }
        payload = self.image + struct.pack("<3I", len(self.image), 0x20020008, 0x20040008)
        header = bytearray(256)
        header[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<I", header, 28, 52)
        struct.pack_into("<HH", header, 42, 32, 1)
        struct.pack_into("<8I", header, 52, 1, 256, address, address, len(payload), len(payload), 4, 256)
        self.elf = self.build / "sbnio.elf"
        self.elf.write_bytes(header + payload)
        binary = bytes(range(256)) + b"final page"
        (self.build / "sbnio.bin").write_bytes(binary)
        (self.build / "sbnio.uf2").write_bytes(compatibility() + firmware(binary))

    def verify(self):
        rows = "\n".join(f"{value:08x} 00000004 R {name}" for name, value in self.symbols.items())
        with patch.object(verifier.subprocess, "check_output", return_value=rows):
            return verifier.verify(self.build, self.rom, "fixture-nm")

    def test_full_verification(self):
        report = self.verify()
        self.assertEqual(report["rom_embedding"], "PASS")
        self.assertEqual(report["uf2_bin_match"], "PASS")
        self.assertFalse(report["hardware_tested"])

    def test_layout_failures(self):
        changes = {
            "ROM source must stay in flash": ("sbnio_rom", 0x20000000),
            "ROM table alignment": ("table_backing", 0x20020004),
            "main-RAM BSS": ("__bss_end__", 0x20040000),
            "main-RAM headroom": ("__HeapLimit", 0x20060001),
            "start in SCRATCH_X": ("sb_persistent_reply", 0x20000000),
            "overlaps Core1 stack": ("__scratch_x_end__", 0x20080804),
            "Core1 stack size": ("__StackOneTop", 0x20080FFC),
            "Core0 stack size": ("__StackTop", 0x20081FFC),
        }
        for message, (name, value) in changes.items():
            with self.subTest(symbol=name), patch.dict(self.symbols, {name: value}):
                with self.assertRaisesRegex(ValueError, message):
                    self.verify()

    def test_rom_embedding_and_size(self):
        self.rom.write_bytes(bytes(len(self.image)))
        with self.assertRaisesRegex(ValueError, "differs from input"):
            self.verify()
        self.rom.write_bytes(self.image[:65536])
        with self.assertRaisesRegex(ValueError, "full 128 KiB"):
            self.verify()

    def test_bad_elf(self):
        original = self.elf.read_bytes()
        for data in (b"", b"not ELF", original[:100]):
            self.elf.write_bytes(data)
            with self.assertRaises(ValueError):
                self.verify()


if __name__ == "__main__":
    unittest.main()
