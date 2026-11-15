"""Small native fixtures for physical section and map accounting."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("pico_size_report", Path(__file__).parents[1] / "pico2w/size_report.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class SizeReportTests(unittest.TestCase):
    def test_map_ignores_discarded_sections_and_counts_data_in_both_memories(self):
        text = """Discarded input sections
 .text.foo 0x10000000 0x999 /src/lib/foo.cpp.o
Linker script and memory map
.text 0x10000000 0x20
 .text.foo 0x10000000 0x20 /src/lib/foo.cpp.o
.data 0x20000000 0x10
 .data.foo
                0x20000000 0x10 /src/platform/pico/foo.cpp.o
.bss 0x20000010 0x8
 .bss.foo 0x20000010 0x8 /src/platform/pico/foo.cpp.o
.stack_dummy 0x2007c000 0x4000
 .stack 0x2007c000 0x4000 /pico-sdk/stack.S.o
"""
        totals = REPORT.map_components(text)
        self.assertEqual(totals["NIO core"], {"flash_bytes": 32, "static_ram_bytes": 0})
        self.assertEqual(totals["NIO platform/app"], {"flash_bytes": 16, "static_ram_bytes": 24})
        self.assertNotIn("Pico SDK/TinyUSB", totals)

    def test_elf_parser_reads_sections_without_executing_anything(self):
        names = b"\0.data\0.bss\0.shstrtab\0"
        header = bytearray(52)
        header[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<H", header, 18, 40)
        struct.pack_into("<I", header, 32, 52)
        struct.pack_into("<HHH", header, 46, 40, 4, 3)
        entries = b"".join(struct.pack("<10I", *fields) for fields in (
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (1, 1, 3, 0x20000100, 0, 16, 0, 0, 4, 0),
            (7, 8, 3, 0x20000110, 0, 32, 0, 0, 4, 0),
            (12, 3, 0, 0, 212, len(names), 0, 0, 1, 0),
        ))
        parsed = REPORT.elf_sections(header + entries + names)
        self.assertEqual(parsed[".data"], (0x20000100, 16, 3))
        self.assertEqual(parsed[".bss"], (0x20000110, 32, 3))
        with self.assertRaises(ValueError):
            REPORT.elf_sections(b"not an ELF")
        with self.assertRaises(ValueError):
            REPORT.map_components("not a map")


if __name__ == "__main__":
    unittest.main()
