#pragma once

namespace fujinet::platform::esp32 {

// Mounts the internal flash filesystem (LittleFS) at /fujifs.
// Returns true on success, false on failure.
bool init_littlefs();

// Mounts the SD card filesystem at /sdcard.
// Returns true on success, false on failure.
bool init_sdcard_spi();

// True while the mounted SD card still answers (a status command to the card), so a card pulled
// from its slot is noticed without waiting for a file operation to fail. A card that has gone is
// unmounted, and an empty slot is retried about once a second, so a card put back is mounted again.
// False if no card is mounted (or the board has no SD pins).
bool sdcard_present() noexcept;

// True if a card is mounted right now. Registration of "sd0" asks this once at start-up, so a
// board with no card (or no SD pins) leaves "sd0" out and "persist:/" lands on flash.
bool sdcard_mounted() noexcept;

} // namespace fujinet::platform::esp32
