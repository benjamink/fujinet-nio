#include "detail/littlefs_volume.h"
#include "fujinet/platform/pico/fs_factory.h"
#include "fujinet/core/logging.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include <array>
#include <cstring>
namespace fujinet::platform::pico {
namespace {
using namespace detail;
struct FlashOperation {
    std::uint32_t offset;
    bool erase;
    std::array<std::uint8_t,page_size> bytes;
};
void execute_flash(void* arg) {
    const auto& op=*static_cast<FlashOperation*>(arg);
    if(op.erase) flash_range_erase(flash_offset+op.offset,block_size);
    else flash_range_program(flash_offset+op.offset,op.bytes.data(),page_size);
}
class HardwareFlash final : public FlashDevice {
public:
    bool read(std::uint32_t offset,void* p,std::size_t n) override {
        if(!flash_range(offset,n)) return false;
        std::memcpy(p,reinterpret_cast<const void*>(XIP_BASE+flash_offset+offset),n); return true;
    }
    bool program(std::uint32_t offset,const void* p,std::size_t n) override {
        if(!flash_range(offset,n,page_size)) return false;
        FlashOperation op{}; op.erase=false;
        auto* bytes=static_cast<const std::uint8_t*>(p);
        for(std::size_t i=0;i<n;i+=page_size) {
            op.offset=offset+i;
            // Input can reside in XIP; copy before flash_safe_execute disables it.
            std::memcpy(op.bytes.data(),bytes+i,page_size);
            if(flash_safe_execute(execute_flash,&op,1000)!=PICO_OK) return false;
        }
        return true;
    }
    bool erase(std::uint32_t offset) override {
        if(!flash_range(offset,block_size,block_size)) return false;
        FlashOperation op{}; op.offset=offset; op.erase=true;
        return flash_safe_execute(execute_flash,&op,1000)==PICO_OK;
    }
};
}
std::unique_ptr<fs::IFileSystem> create_flash_filesystem() {
    auto fs=detail::mount_littlefs(std::make_shared<HardwareFlash>());
    if(!fs) FN_LOGE("flash","LittleFS mount failed; existing nonblank storage was not formatted");
    return fs;
}
}
