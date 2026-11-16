#pragma once
#include "fujinet/fs/filesystem.h"
#include <cstddef>
#include <cstdint>
#include <memory>
namespace fujinet::platform::pico::detail {
inline constexpr std::uint32_t flash_offset=3*1024*1024;
inline constexpr std::uint32_t flash_length=1024*1024;
inline constexpr std::uint32_t block_size=4096;
inline constexpr std::uint32_t page_size=256;
constexpr bool flash_range(std::uint32_t offset,std::size_t length,std::size_t alignment=1) {
    return alignment && offset <= flash_length && length <= flash_length-offset &&
           offset % alignment == 0 && length % alignment == 0;
}
// All offsets are relative to the reserved partition. Hardware and native RAM
// devices enforce the same bounds; the volume never sees an XIP address.
class FlashDevice {
public:
    virtual ~FlashDevice()=default;
    virtual bool read(std::uint32_t offset,void* out,std::size_t n)=0;
    virtual bool program(std::uint32_t offset,const void* data,std::size_t n)=0;
    virtual bool erase(std::uint32_t offset)=0;
};
std::unique_ptr<fs::IFileSystem> mount_littlefs(std::shared_ptr<FlashDevice> device);
// Recreate an empty LittleFS superblock, whatever the partition holds. Only the
// operator-confirmed console `format` command calls this: mount_littlefs never
// formats nonblank storage by itself. Other blocks are not wiped; LittleFS erases
// a block before it writes it.
bool format_littlefs(std::shared_ptr<FlashDevice> device);
}
