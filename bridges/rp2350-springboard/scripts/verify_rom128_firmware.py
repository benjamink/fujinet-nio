#!/usr/bin/env python3
"""Verify diagnostic ELF placement, ROM embedding and UF2/BIN agreement."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def verify(build, rom, nm):
    elf = (build / "sbnio.elf").read_bytes()
    assert elf[:6] == b"\x7fELF\x01\x01", "expected little-endian ELF32"
    rows = subprocess.check_output([nm, "-S", "-n", str(build / "sbnio.elf")], text=True)
    symbols = {}
    for row in rows.splitlines():
        fields = row.split()
        if len(fields) in (3, 4):
            symbols[fields[-1]] = int(fields[0], 16)
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phsize, phcount = struct.unpack_from("<HH", elf, 42)

    def content(address, size):
        for index in range(phcount):
            kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", elf, phoff + phsize * index)
            if kind == 1 and vaddr <= address and address + size <= vaddr + filesz:
                return elf[offset + address - vaddr:offset + address - vaddr + size]
        raise ValueError("unbacked ELF address %08x" % address)

    image = rom.read_bytes()
    assert len(image) == 0x20000
    assert 0x10000000 <= symbols["sbnio_rom"] < 0x11000000, "ROM source must stay in flash"
    assert content(symbols["sbnio_rom"], len(image)) == image
    assert struct.unpack("<I", content(symbols["sbnio_rom_size"], 4))[0] == len(image)
    words = struct.unpack("<I", content(symbols["bus_rom_words"], 4))[0]
    cs1 = struct.unpack("<I", content(symbols["bus_cs1_words"], 4))[0]
    assert words == symbols["table_backing"] + 8 and words % 16 == 8
    assert cs1 == words + 0x20000
    assert 0x20000000 <= words < cs1 and cs1 + 0x10000 <= symbols["__bss_end__"]
    headroom = symbols["__HeapLimit"] - symbols["__bss_end__"]
    assert headroom >= 65536, "less than 64 KiB main-RAM headroom"
    assert symbols["sb_persistent_reply"] == 0x20080000
    assert symbols["__scratch_x_end__"] <= symbols["__StackOneBottom"]
    assert symbols["__StackOneTop"] - symbols["__StackOneBottom"] == 2048
    assert symbols["__StackTop"] - symbols["__StackBottom"] == 4096
    binary = (build / "sbnio.bin").read_bytes()
    uf2 = (build / "sbnio.uf2").read_bytes()
    assert len(uf2) % 512 == 0
    payloads = {}
    for offset in range(0, len(uf2), 512):
        m0, m1, flags, address, count, block, total, family = struct.unpack_from("<8I", uf2, offset)
        assert (m0, m1) == (0x0a324655, 0x9e5d5157)
        assert struct.unpack_from("<I", uf2, offset + 508)[0] == 0x0ab16f30
        if flags & 0x8000:  # RP2350 family-selection block, not firmware bytes
            continue
        assert flags == 0x2000 and family == 0xe48bff59 and count == 256
        assert address == 0x10000000 + 256 * block
        assert block not in payloads
        payloads[block] = uf2[offset + 32:offset + 32 + count]
        assert total == (len(binary) + 255) // 256
    assert set(payloads) == set(range((len(binary) + 255) // 256))
    restored = b"".join(payloads[index] for index in range(len(payloads)))
    assert restored[:len(binary)] == binary
    result = {"rom_source_address": hex(symbols["sbnio_rom"]), "rom_table_address": hex(words),
              "cs1_table_address": hex(cs1), "main_ram_headroom_bytes": headroom,
              "core1_stack_bytes": 2048, "core0_stack_bytes": 4096,
              "rom_embedding": "PASS", "uf2_bin_match": "PASS", "hardware_tested": False}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    args = parser.parse_args()
    report = verify(args.build, args.rom, args.nm)
    (args.build / "rom128-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
