#!/usr/bin/env python3
"""Verify diagnostic ELF placement, ROM embedding and UF2/BIN agreement."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

# Pinned Pico SDK boot/uf2.h and picotool's RP2350-E10 compatibility block.
UF2_MAGIC = (0x0A324655, 0x9E5D5157)
UF2_END = 0x0AB16F30
UF2_FAMILY_PRESENT = 0x2000
UF2_EXTENSION_PRESENT = 0x8000
RP2350_ARM_S_FAMILY = 0xE48BFF59
ABSOLUTE_FAMILY = 0xE48BFF57
RP2_IGNORE_BLOCK = 0x9957E304
FLASH_BASE, FLASH_END = 0x10000000, 0x11000000
PAGE_BYTES = 256


def require(condition, message):
    # Verification must remain active with Python optimization enabled.
    if not condition:
        raise ValueError(message)


def verify_uf2(uf2, binary):
    require(bool(binary), "BIN is empty")
    require(len(binary) <= FLASH_END - FLASH_BASE, "BIN exceeds the flash window")
    require(bool(uf2) and len(uf2) % 512 == 0, "UF2 must contain complete 512-byte blocks")
    expected = (len(binary) + PAGE_BYTES - 1) // PAGE_BYTES
    payloads = {}
    compatibility_seen = False
    for offset in range(0, len(uf2), 512):
        m0, m1, flags, address, count, block, total, family = struct.unpack_from("<8I", uf2, offset)
        require((m0, m1) == UF2_MAGIC, "invalid UF2 start magic")
        require(struct.unpack_from("<I", uf2, offset + 508)[0] == UF2_END, "invalid UF2 end magic")
        require(count == PAGE_BYTES, "expected 256-byte UF2 payload")
        payload = uf2[offset + 32:offset + 32 + count]
        tail = uf2[offset + 32 + count:offset + 508]
        if family == ABSOLUTE_FAMILY:
            # Only this exact optional metadata block is excluded from BIN data.
            # Extension-tagged firmware is never silently skipped.
            require(not compatibility_seen, "duplicate RP2350 compatibility block")
            require(offset == 0, "RP2350 compatibility block must precede firmware")
            require(flags == (UF2_FAMILY_PRESENT | UF2_EXTENSION_PRESENT),
                    "invalid RP2350 compatibility flags")
            require(address == FLASH_END - PAGE_BYTES, "invalid RP2350 compatibility address")
            require(block == 0 and total == 2, "invalid RP2350 compatibility numbering")
            require(payload == b"\xef" * PAGE_BYTES, "invalid RP2350 compatibility payload")
            require(tail == struct.pack("<I", RP2_IGNORE_BLOCK) + bytes(len(tail) - 4),
                    "invalid RP2350 ignore-block extension")
            compatibility_seen = True
            continue
        require(family == RP2350_ARM_S_FAMILY, "unexpected UF2 firmware family")
        require(flags == UF2_FAMILY_PRESENT, "unsupported UF2 firmware flags")
        require(block < expected and total == expected, "invalid UF2 firmware numbering")
        require(address == FLASH_BASE + PAGE_BYTES * block, "invalid UF2 firmware address")
        require(block not in payloads, "duplicate UF2 firmware block")
        require(tail == bytes(len(tail)), "unexpected UF2 firmware metadata")
        payloads[block] = payload
    require(set(payloads) == set(range(expected)), "missing UF2 firmware blocks")
    restored = b"".join(payloads[index] for index in range(expected))
    require(restored[:len(binary)] == binary, "UF2 firmware differs from BIN")
    require(restored[len(binary):] == bytes(len(restored) - len(binary)), "nonzero UF2 final-page padding")


def verify(build, rom, nm):
    elf = (build / "sbnio.elf").read_bytes()
    require(len(elf) >= 52 and elf[:6] == b"\x7fELF\x01\x01", "expected little-endian ELF32")
    rows = subprocess.check_output([nm, "-S", "-n", str(build / "sbnio.elf")], text=True)
    symbols = {}
    for row in rows.splitlines():
        fields = row.split()
        if len(fields) in (3, 4):
            symbols[fields[-1]] = int(fields[0], 16)
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phsize, phcount = struct.unpack_from("<HH", elf, 42)
    require(phsize >= 32 and phoff + phsize * phcount <= len(elf), "invalid ELF program headers")

    def content(address, size):
        for index in range(phcount):
            kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", elf, phoff + phsize * index)
            if kind == 1 and vaddr <= address and address + size <= vaddr + filesz:
                start = offset + address - vaddr
                require(start + size <= len(elf), "truncated ELF segment")
                return elf[start:start + size]
        raise ValueError("unbacked ELF address %08x" % address)

    image = rom.read_bytes()
    require(len(image) == 0x20000, "verification requires a full 128 KiB ROM")
    require(FLASH_BASE <= symbols["sbnio_rom"] and symbols["sbnio_rom"] + len(image) <= FLASH_END,
            "ROM source must stay in flash")
    require(content(symbols["sbnio_rom"], len(image)) == image, "ELF ROM differs from input")
    require(struct.unpack("<I", content(symbols["sbnio_rom_size"], 4))[0] == len(image), "incorrect ROM size")
    words = struct.unpack("<I", content(symbols["bus_rom_words"], 4))[0]
    cs1 = struct.unpack("<I", content(symbols["bus_cs1_words"], 4))[0]
    require(words == symbols["table_backing"] + 8 and words % 16 == 8, "incorrect ROM table alignment")
    require(cs1 == words + 0x20000, "CS1 table must immediately follow the ROM table")
    require(0x20000000 <= words < cs1 and cs1 + 0x10000 <= symbols["__bss_end__"],
            "ROM and CS1 tables must fit in main-RAM BSS")
    headroom = symbols["__HeapLimit"] - symbols["__bss_end__"]
    require(headroom >= 65536, "less than 64 KiB main-RAM headroom")
    require(symbols["sb_persistent_reply"] == 0x20080000, "responder must start in SCRATCH_X")
    require(symbols["__scratch_x_end__"] <= symbols["__StackOneBottom"], "responder overlaps Core1 stack")
    require(symbols["__StackOneTop"] - symbols["__StackOneBottom"] == 2048, "incorrect Core1 stack size")
    require(symbols["__StackTop"] - symbols["__StackBottom"] == 4096, "incorrect Core0 stack size")
    verify_uf2((build / "sbnio.uf2").read_bytes(), (build / "sbnio.bin").read_bytes())
    return {"rom_source_address": hex(symbols["sbnio_rom"]), "rom_table_address": hex(words),
            "cs1_table_address": hex(cs1), "main_ram_headroom_bytes": headroom,
            "core1_stack_bytes": 2048, "core0_stack_bytes": 4096,
            "rom_embedding": "PASS", "uf2_bin_match": "PASS", "hardware_tested": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    args = parser.parse_args()
    try:
        report = verify(args.build, args.rom, args.nm)
    except (ValueError, KeyError, OSError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f"verification failed: {error}\n")
    (args.build / "rom128-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
