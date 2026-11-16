#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/platform/pico/detail/littlefs_volume.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace fujinet::platform::pico::detail;
struct RamFlash final:FlashDevice {
    std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(flash_length,0xff);
    bool fail_read=false,fail_program=false,fail_erase=false;
    unsigned programs=0,erases=0;
    bool read(std::uint32_t offset,void* out,std::size_t n) override {
        REQUIRE(flash_range(offset,n));
        if(fail_read)return false;
        std::memcpy(out,bytes.data()+offset,n);return true;
    }
    bool program(std::uint32_t offset,const void* input,std::size_t n) override {
        REQUIRE(flash_range(offset,n,page_size));
        if(fail_program)return false;
        auto* data=static_cast<const std::uint8_t*>(input);
        for(std::size_t i=0;i<n;++i) {
            if((bytes[offset+i]&data[i])!=data[i])return false;
            bytes[offset+i]&=data[i];
        }
        ++programs;return true;
    }
    bool erase(std::uint32_t offset) override {
        REQUIRE(flash_range(offset,block_size,block_size));
        if(fail_erase)return false;
        std::fill_n(bytes.begin()+offset,block_size,0xff);++erases;return true;
    }
};
}
TEST_CASE("LittleFS formats blank partition and persists config across remount") {
    auto device=std::make_shared<RamFlash>();
    auto fs=mount_littlefs(device);REQUIRE(fs);CHECK(fs->name()=="flash");
    CHECK(fs->kind()==fujinet::fs::FileSystemKind::LocalFlash);
    const std::string yaml="wifi:\n  ssid: fixture\n";
    {
        auto file=fs->open("/fujinet.yaml","wb");REQUIRE(file);
        CHECK(file->write(yaml.data(),yaml.size())==yaml.size());REQUIRE(file->flush());
    }
    fs.reset();const auto erases=device->erases;const auto programs=device->programs;
    fs=mount_littlefs(device);REQUIRE(fs);
    CHECK(device->erases==erases);CHECK(device->programs==programs);
    fujinet::fs::FileInfo info;REQUIRE(fs->stat("/fujinet.yaml",info));CHECK(info.sizeBytes==yaml.size());
    auto file=fs->open("/fujinet.yaml","rb");REQUIRE(file);
    std::array<char,64> out{};CHECK(file->read(out.data(),out.size())==yaml.size());
    CHECK(std::string(out.data(),yaml.size())==yaml);
    CHECK(file->read(out.data(),out.size())==0);
    CHECK_FALSE(file->seek(std::numeric_limits<std::uint64_t>::max()));
    // An open file owns the mounted volume even if the registry releases its FS.
    fs.reset();CHECK(file->seek(0));CHECK(file->read(out.data(),out.size())==yaml.size());
}
TEST_CASE("LittleFS refuses damage in either superblock copy without erasing") {
    // Data in block 0 or 1 means a volume existed there: never format over it.
    for(auto offset:{0u,1u,block_size-1,block_size,2*block_size-1}) {
        CAPTURE(offset);
        auto device=std::make_shared<RamFlash>();device->bytes[offset]=0;
        const auto before=device->bytes;
        CHECK_FALSE(mount_littlefs(device));CHECK(device->erases==0);CHECK(device->programs==0);
        CHECK(device->bytes==before);
    }
}
TEST_CASE("LittleFS formats when only the superblock copies are blank") {
    // Seen on a stock Pico 2 W: a single 0xEF page at the very end of flash. It must not
    // disable storage, and nothing outside blocks 0 and 1 may be judged.
    for(auto offset:{2*block_size,block_size*3+17,flash_length/2,flash_length-1,flash_length-page_size}) {
        CAPTURE(offset);
        auto device=std::make_shared<RamFlash>();
        std::fill_n(device->bytes.begin()+offset,1,0xef);
        auto fs=mount_littlefs(device);REQUIRE(fs);
        const std::string yaml="wifi:\n  ssid: stock\n";
        {
            auto file=fs->open("/fujinet.yaml","wb");REQUIRE(file);
            CHECK(file->write(yaml.data(),yaml.size())==yaml.size());REQUIRE(file->flush());
        }
        fs.reset();
        fs=mount_littlefs(device);REQUIRE(fs);            // persists across a remount
        fujinet::fs::FileInfo info;REQUIRE(fs->stat("/fujinet.yaml",info));CHECK(info.sizeBytes==yaml.size());
    }
    // A whole stray page, as seen on hardware.
    auto device=std::make_shared<RamFlash>();
    std::fill(device->bytes.end()-page_size,device->bytes.end(),0xef);
    CHECK(mount_littlefs(device));
}
TEST_CASE("LittleFS read and formatting failures never produce a mounted filesystem") {
    auto device=std::make_shared<RamFlash>();
    device->fail_read=true;CHECK_FALSE(mount_littlefs(device));CHECK(device->erases==0);
    device->fail_read=false;device->fail_erase=true;CHECK_FALSE(mount_littlefs(device));
    device->fail_erase=false;device->fail_program=true;CHECK_FALSE(mount_littlefs(device));
}
TEST_CASE("LittleFS supports directories append rename and rejects traversal and bad modes") {
    auto device=std::make_shared<RamFlash>();auto fs=mount_littlefs(device);REQUIRE(fs);
    REQUIRE(fs->createDirectory("/dir"));
    {
        auto f=fs->open("/dir/a","wb");REQUIRE(f);REQUIRE(f->write("abc",3)==3);REQUIRE(f->flush());
    }
    {
        auto f=fs->open("/dir/a","ab");REQUIRE(f);REQUIRE(f->write("def",3)==3);REQUIRE(f->flush());
    }
    CHECK(fs->rename("/dir/a","/dir/b"));CHECK_FALSE(fs->exists("/dir/a"));
    std::vector<fujinet::fs::FileInfo> entries;REQUIRE(fs->listDirectory("/dir",entries));
    REQUIRE(entries.size()==1);CHECK(entries[0].path=="/dir/b");CHECK(entries[0].sizeBytes==6);
    CHECK_FALSE(fs->open("/../escape","wb"));CHECK_FALSE(fs->open("/dir/../../escape","wb"));
    CHECK_FALSE(fs->open(std::string("/nul\0hidden",11),"wb"));
    CHECK_FALSE(fs->open("/x","rx"));CHECK_FALSE(fs->open("/x","w++"));
    CHECK_FALSE(fs->removeDirectory("/dir"));CHECK_FALSE(fs->removeFile("/dir"));
    CHECK(fs->removeFile("/dir/b"));CHECK(fs->removeDirectory("/dir"));
}
TEST_CASE("Pico flash range rejects overflow and misaligned program or erase") {
    CHECK(flash_range(0,flash_length));CHECK(flash_range(flash_length,0));
    CHECK(flash_range(flash_length-page_size,page_size,page_size));
    CHECK_FALSE(flash_range(flash_length,1));
    CHECK_FALSE(flash_range(std::numeric_limits<std::uint32_t>::max(),1));
    CHECK_FALSE(flash_range(1,std::numeric_limits<std::size_t>::max()));
    CHECK_FALSE(flash_range(1,page_size,page_size));CHECK_FALSE(flash_range(0,page_size-1,page_size));
    CHECK_FALSE(flash_range(page_size,block_size,block_size));CHECK_FALSE(flash_range(0,0,0));
}

TEST_CASE("format recovers a damaged superblock and reports flash failures") {
    auto device=std::make_shared<RamFlash>();
    REQUIRE(mount_littlefs(device));                  // blank: formatted automatically
    std::fill_n(device->bytes.begin(),64,0x00);       // damage the first superblock copy
    std::fill_n(device->bytes.begin()+block_size,64,0x00);   // and its pair
    CHECK_FALSE(mount_littlefs(device));              // damaged data is never formatted over
    REQUIRE(format_littlefs(device));
    CHECK(mount_littlefs(device));

    auto failing=std::make_shared<RamFlash>();
    failing->fail_erase=true;
    CHECK_FALSE(format_littlefs(failing));            // a failed erase is a failed format
    CHECK_FALSE(format_littlefs(nullptr));
}
