#pragma once
#include <cstdint>
bool tusb_init();
void tud_task();
bool tud_cdc_connected();
void tud_cdc_read_flush();
void tud_cdc_write_clear();
std::uint32_t tud_cdc_read(void*,std::uint32_t);
std::uint32_t tud_cdc_write_available();
std::uint32_t tud_cdc_write(const void*,std::uint32_t);
std::uint32_t tud_cdc_write_flush();
