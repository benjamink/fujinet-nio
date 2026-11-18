/* SDK-independent storage/loader, shared by firmware and native bounds tests. */
#include "bus.h"

/* Keep the proven modulo-16 SRAM bank alignment. CS1 immediately follows ROM,
 * as assumed by persistent_reply.S, but retains its independent 64 KiB size. */
static uint8_t table_backing[BUS_ROM_BYTES + BUS_CS1_BYTES + 16]
    __attribute__((aligned(16)));
uint16_t *const bus_rom_words = (uint16_t *)(table_backing + 8);
uint16_t *const bus_cs1_words =
    (uint16_t *)(table_backing + 8) + BUS_ROM_WORDS;

void bus_load_rom(const uint8_t *rom, uint32_t size) {
    for (uint32_t i = 0; i < BUS_ROM_WORDS; ++i) {
        uint32_t a = 2 * i;
        uint16_t hi = a < size ? rom[a] : 0xff,
                 lo = a + 1 < size ? rom[a + 1] : 0xff;
        bus_rom_words[i] = (uint16_t)(hi << 8 | lo);
    }
}
