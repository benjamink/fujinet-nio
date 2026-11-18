#!/usr/bin/env python3
"""Execute the real ARM responder, mocking only MMIO (not bus timing).

Requires arm-none-eabi-{gcc,ld,objcopy,nm} and Python unicorn==2.1.4.
Run explicitly, or set SPRINGBOARD_ARM_TEST_PYTHON when configuring host tests.
"""
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR

ROOT = Path(__file__).resolve().parents[1]
CODE, TABLE, STATE, MMIO, SIO = 0x20080000, 0x20000008, 0x20070000, 0x50000000, 0x50001000


class ReplyARM(unittest.TestCase):
    def test_real_responder(self):
        toolchain = Path(os.environ.get("PICO_TOOLCHAIN_PATH", "/usr/bin"))

        def tool(name):
            path = toolchain / ("arm-none-eabi-" + name)
            return str(path) if path.exists() else shutil.which("arm-none-eabi-" + name)

        with tempfile.TemporaryDirectory() as temp:
            obj, elf, binary = [Path(temp) / name for name in ("reply.o", "reply.elf", "reply.bin")]
            subprocess.run([tool("gcc"), "-mcpu=cortex-m33", "-mthumb", "-c",
                            str(ROOT / "src/persistent_reply.S"), "-o", str(obj)], check=True)
            subprocess.run([tool("ld"), "--section-start=.scratch_x.sb_persistent_reply=" + hex(CODE),
                            "-e", "sb_persistent_reply", str(obj), "-o", str(elf)], check=True)
            subprocess.run([tool("objcopy"), "-O", "binary", "-j", ".scratch_x.sb_persistent_reply",
                            str(elf), str(binary)], check=True)
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
            uc.mem_map(0x20000000, 0x90000)
            uc.mem_map(MMIO, 0x2000)
            uc.mem_write(CODE, binary.read_bytes())
            # Unique lower/upper values at every matching offset; separate CS1.
            words = [((i * 73) ^ (i >> 15) ^ 0x5aa5) & 0xffff for i in range(65536)]
            cs1 = [((i * 19) ^ 0xa55a) & 0xffff for i in range(32768)]
            uc.mem_write(TABLE, struct.pack("<98304H", *(words + cs1)))
            published = []
            high_snapshot = 0

            def read_mmio(engine, access, address, size, value, data):
                if address == MMIO + 4:  # RX FIFO ready
                    engine.mem_write(address, bytes(4))
                elif address == MMIO + 0x20:
                    engine.mem_write(address, struct.pack("<I", high_snapshot))

            def write_mmio(engine, access, address, size, value, data):
                if address == MMIO + 0x10:
                    published.append(value)
                    engine.mem_write(STATE + 64, b"\x01")  # stop after publication

            uc.hook_add(UC_HOOK_MEM_READ, read_mmio, begin=MMIO, end=MMIO + 0xfff)
            uc.hook_add(UC_HOOK_MEM_WRITE, write_mmio, begin=MMIO, end=MMIO + 0xfff)

            def run(address, select=0, valid=True):
                nonlocal high_snapshot
                high_snapshot = 0x20000000 | (9 if select else 10)
                high_snapshot |= (address & 0x70000) << 4
                high_snapshot |= (address & 0xf80000) << 5
                if not valid:
                    high_snapshot &= ~0x20000000  # RESET low
                # Live high GPIO deliberately disagrees with the owned snapshot.
                uc.mem_write(SIO, struct.pack("<II", address & 0xfffe, ~high_snapshot & 0xffffffff))
                uc.mem_write(STATE, bytes(128))
                uc.mem_write(STATE + 32, struct.pack("<III", MMIO, SIO, STATE + 64))
                uc.reg_write(UC_ARM_REG_R0, STATE)
                uc.reg_write(UC_ARM_REG_R1, TABLE)
                uc.reg_write(UC_ARM_REG_R2, STATE + 32)
                uc.reg_write(UC_ARM_REG_SP, STATE + 0xff0)
                uc.reg_write(UC_ARM_REG_LR, (CODE + 0xff0) | 1)
                published.clear()
                uc.emu_start(CODE | 1, CODE + 0xff0, count=250)
                expected = (cs1[(address & 0xffff) // 2] if select else
                            words[address // 2] if address < 0x20000 else 0xffff) if valid else 0
                self.assertEqual(published, [expected], (hex(address), select, valid))
                self.assertEqual(struct.unpack("<I", uc.mem_read(STATE, 4))[0], 1)

            # Every CS0 word, both halves, plus high-address rejection and CS1 aliases.
            for address in range(0, 0x20000, 2):
                run(address)
            rng = random.Random(128)
            for address in [0, 0xfffe, 0x10000, 0x1fffe, 0x20000, 0xffffff & ~1] + [1 << bit for bit in range(17, 24)]:
                run(address)
                run(address, select=1)
                run(address, valid=False)
            for _ in range(256):
                run(rng.randrange(0, 0x800000) * 2, select=1)
            print("Actual ARM responder: all 65,536 ROM words, high-address rejection, CS1 aliases and RESET PASS")


if __name__ == "__main__":
    unittest.main()
