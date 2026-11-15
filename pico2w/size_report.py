#!/usr/bin/env python3
"""Report Pico flash/static RAM without executing the ARM ELF.

Pico's linker marks initialized RAM as executable, so Berkeley `size` columns
are misleading. Physical flash comes from the binary; static RAM comes from ELF
allocated sections. Map input-section components omit alignment/padding.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import struct

RAM_START, RAM_END = 0x20000000, 0x20082000
FLASH_START, FLASH_END = 0x10000000, 0x10400000
COMPONENT_SECTIONS = {
    ".text", ".rodata", ".ARM.extab", ".ARM.exidx", ".binary_info", ".data",
    ".bss", ".ram_vector_table", ".uninitialized_data", ".flash_end",
}


def component_name(obj):
    for fragment, name in (
        ("libstdc++", "libstdc++"), ("yaml-cpp", "yaml-cpp"), ("cyw43", "CYW43"),
        ("/lwip/", "lwIP"), ("/mbedtls/", "mbedTLS"), ("/littlefs/", "LittleFS"),
        ("/src/lib/", "NIO core"), ("/src/platform/", "NIO platform/app"),
        ("/src/app/", "NIO platform/app"), ("/pico-sdk/", "Pico SDK/TinyUSB"),
        (".a(", "newlib/libgcc/other archives"),
    ):
        if fragment in obj:
            return name
    return "other"


def map_components(text):
    marker = "Linker script and memory map"
    if marker not in text:
        raise ValueError("map has no allocated-section marker")
    totals = defaultdict(lambda: {"flash_bytes": 0, "static_ram_bytes": 0})
    section = ""
    for line in text.split(marker, 1)[1].splitlines():
        output = re.match(r"^(\.[\w.]+)\s+0x[0-9a-f]+\s+0x[0-9a-f]+", line)
        if output:
            section = output[1]
        if section not in COMPONENT_SECTIONS:
            continue
        entry = re.match(
            r"^\s+(?:\.[^\s]+\s+)?0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o(?:\))?)$", line)
        if not entry:
            continue
        address, size, obj = int(entry[1], 16), int(entry[2], 16), entry[3]
        values = totals[component_name(obj)]
        if FLASH_START <= address < FLASH_END or section == ".data":
            values["flash_bytes"] += size
        if RAM_START <= address < RAM_END:
            values["static_ram_bytes"] += size
    return dict(sorted(totals.items()))


def elf_sections(data):
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected a little-endian 32-bit ELF")
    if struct.unpack_from("<H", data, 18)[0] != 40:
        raise ValueError("expected ARM ELF (the file is parsed, never run)")
    offset = struct.unpack_from("<I", data, 32)[0]
    stride, count, strings_index = struct.unpack_from("<HHH", data, 46)
    if stride < 40 or not count or strings_index >= count or offset + stride * count > len(data):
        raise ValueError("invalid ELF section table")
    entries = [struct.unpack_from("<10I", data, offset + i * stride) for i in range(count)]
    string_offset, string_size = entries[strings_index][4:6]
    strings = data[string_offset:string_offset + string_size]
    result = {}
    for name, _, flags, address, _, size, *_ in entries:
        if name >= len(strings):
            raise ValueError("invalid ELF section name")
        end = strings.find(b"\0", name)
        if end < 0:
            raise ValueError("unterminated ELF section name")
        result[strings[name:end].decode("ascii")] = (address, size, flags)
    return result


def report(map_path, elf_path, binary_path):
    components = map_components(map_path.read_text())
    sections = elf_sections(elf_path.read_bytes())
    ram = {name: size for name, (address, size, flags) in sections.items()
           if flags & 2 and size and RAM_START <= address < RAM_END}
    stack = sum(size for name, size in ram.items() if name.startswith(".stack"))
    heap = ram.get(".heap", 0)
    static = sum(size for name, size in ram.items() if name != ".heap" and not name.startswith(".stack"))
    return {
        "flash_binary_bytes": binary_path.stat().st_size,
        "static_ram_bytes": static,
        "minimum_heap_reservation_bytes": heap,
        "stack_reservation_bytes": stack,
        "runtime_tls_heap_peak_bytes": None,
        "runtime_stack_peak_bytes": None,
        "components_excluding_padding": components,
        "note": "Static lower bound only; no hardware or runtime peak measurements.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path, help="Pico firmware .elf.map")
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    elf = args.elf or args.map.with_suffix("")
    binary = args.binary or elf.with_suffix(".bin")
    try:
        result = report(args.map, elf, binary)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(result, indent=2))
        return
    for key in ("flash_binary_bytes", "static_ram_bytes", "minimum_heap_reservation_bytes", "stack_reservation_bytes"):
        print(f"{key}: {result[key]}")
    print("component,flash_input_bytes,static_ram_input_bytes")
    for name, values in result["components_excluding_padding"].items():
        print(f"{name},{values['flash_bytes']},{values['static_ram_bytes']}")
    print(result["note"])


if __name__ == "__main__":
    main()
