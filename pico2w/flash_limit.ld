/* Supplement the SDK linker script: firmware must never overlap LittleFS. */
SECTIONS { .flash_partition_guard : { ASSERT(__flash_binary_end <= 0x10300000, "Firmware overlaps reserved LittleFS partition"); } }
INSERT AFTER .flash_end;
