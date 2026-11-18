#include "bus.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

int main(void) {
    const unsigned sizes[] = {0, 512, 65536, 65538, BUS_ROM_BYTES};
    CHECK(bus_cs1_words == bus_rom_words + BUS_ROM_WORDS);
    CHECK(((uintptr_t)bus_rom_words & 15) == 8);
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        unsigned size = sizes[s];
        uint8_t *image = size ? malloc(size) : NULL;
        CHECK(!size || image);
        for (unsigned i = 0; i < size; ++i)
            image[i] = (uint8_t)((i * 73u) ^ (i >> 8) ^ (i >> 16));
        for (unsigned i = 0; i < BUS_CS1_WORDS; ++i)
            bus_cs1_words[i] = (uint16_t)(i ^ 0xa55a);
        bus_load_rom(image, size);
        for (unsigned i = 0; i < BUS_ROM_WORDS; ++i) {
            unsigned address = 2 * i;
            uint16_t expected = address < size
                ? (uint16_t)((image[address] << 8) | image[address + 1]) : 0xffff;
            CHECK(bus_rom_words[i] == expected);
        }
        for (unsigned i = 0; i < BUS_CS1_WORDS; ++i)
            CHECK(bus_cs1_words[i] == (uint16_t)(i ^ 0xa55a));
        free(image);
    }
    puts("ROM table: endian conversion, 64/128 KiB bounds, erased tail and CS1 isolation PASS");
    return 0;
}
