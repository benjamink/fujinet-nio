#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/platform/pico/detail/usb_bridge.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
using fujinet::platform::pico::detail::UsbBridge;
struct Device {
    std::deque<std::uint8_t> input;
    std::vector<std::uint8_t> output;
    std::size_t accept=64, space=64, reads=0, clears=0;
    void clear_rx() { input.clear(); ++clears; }
    void clear_tx() {}
    std::size_t read(void* p,std::size_t n) {
        ++reads; n=std::min(n,input.size());
        for(std::size_t i=0;i<n;++i) { static_cast<std::uint8_t*>(p)[i]=input.front(); input.pop_front(); }
        return n;
    }
    std::size_t write_available() { return space; }
    std::size_t write(const void* p,std::size_t n) {
        n=std::min(n,accept); auto bytes=static_cast<const std::uint8_t*>(p);
        output.insert(output.end(),bytes,bytes+n); return n;
    }
    void flush() {}
};
TEST_CASE("RX full retains CDC bytes and resumes in order") {
    UsbBridge<std::mutex,4,8> bridge; Device device;
    REQUIRE(bridge.attach()); CHECK_FALSE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    device.input={1,2,3,4,5,6}; bridge.pump(device);
    CHECK(device.input.size()==2); auto reads=device.reads;
    bridge.pump(device); CHECK(device.reads==reads);
    std::uint8_t bytes[8]; REQUIRE(bridge.read(bytes,3)==3);
    CHECK(std::vector<std::uint8_t>(bytes,bytes+3)==std::vector<std::uint8_t>{1,2,3});
    bridge.pump(device); REQUIRE(bridge.read(bytes,8)==3);
    CHECK(std::vector<std::uint8_t>(bytes,bytes+3)==std::vector<std::uint8_t>{4,5,6});
}
TEST_CASE("short and zero submissions preserve bytes and count toward capacity") {
    UsbBridge<std::mutex,4,8> bridge; Device device;
    REQUIRE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    std::uint8_t bytes[]={1,2,3,4,5,6,7,8}; REQUIRE(bridge.write(bytes,8));
    device.accept=0; bridge.pump(device); CHECK(device.output.empty());
    device.accept=3; bridge.pump(device); REQUIRE(bridge.write(bytes,3));
    bridge.pump(device); bridge.pump(device); bridge.pump(device);
    CHECK(device.output==std::vector<std::uint8_t>{1,2,3,4,5,6,7,8,1,2,3});
}
TEST_CASE("overflow atomically quarantines RX TX and staged output until reconnect") {
    UsbBridge<std::mutex,4,8> bridge; Device device;
    REQUIRE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    std::uint8_t bytes[]={1,2,3,4,5,6,7,8}; REQUIRE(bridge.write(bytes,8));
    device.input={1}; device.accept=0; bridge.pump(device); REQUIRE(bridge.available());
    auto epoch=bridge.epoch(); CHECK_FALSE(bridge.write(bytes,1)); CHECK(bridge.epoch()!=epoch);
    CHECK_FALSE(bridge.available()); CHECK(bridge.read(bytes,8)==0);
    device.accept=64; bridge.pump(device); CHECK(device.output.empty());
    bridge.connection(true); CHECK_FALSE(bridge.write(bytes,1));
    bridge.connection(false); bridge.connection(true); REQUIRE(bridge.write(bytes,2));
    bridge.pump(device); CHECK(device.output==std::vector<std::uint8_t>{1,2});
}
TEST_CASE("disconnect reset and detached channels cannot retain staged session data") {
    UsbBridge<std::mutex,4,8> bridge; Device device;
    REQUIRE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    std::uint8_t bytes[]={1,2,3}; REQUIRE(bridge.write(bytes,3));
    device.accept=0; device.input={4}; bridge.pump(device);
    auto epoch=bridge.epoch(); bridge.reset(); CHECK(bridge.epoch()!=epoch);
    CHECK_FALSE(bridge.available()); bridge.connection(true); device.accept=64;
    bridge.pump(device); CHECK(device.output.empty());
    REQUIRE(bridge.write(bytes,3)); device.accept=0; bridge.pump(device);
    epoch=bridge.epoch(); bridge.detach(); CHECK(bridge.epoch()!=epoch);
    CHECK_FALSE(bridge.write(bytes,1)); REQUIRE(bridge.attach());
    device.accept=64; bridge.pump(device); CHECK(device.output.empty());
    REQUIRE(bridge.write(bytes,1)); bridge.pump(device); CHECK(device.output.size()==1);
}
TEST_CASE("two threads exchange ordered bounded RX TX independently") {
    UsbBridge<std::mutex,256,256> bridge; Device device;
    REQUIRE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    constexpr unsigned count=100000;
    std::atomic<unsigned> received{0};
    std::atomic<bool> done{false};
    bool ordered=true;
    std::thread worker([&] {
        unsigned supplied=0;
        while(!done.load()) {
            if(device.input.empty() && supplied<count) {
                for(unsigned i=0;i<64 && supplied<count;++i,++supplied)
                    device.input.push_back(static_cast<std::uint8_t>(supplied));
            }
            bridge.pump(device);
            received.store(device.output.size());
            std::this_thread::yield();
        }
    });
    unsigned input=0,output=0;
    while(input<count || output<count) {
        std::uint8_t bytes[64]; auto n=bridge.read(bytes,sizeof(bytes));
        for(std::size_t i=0;i<n;++i,++input) ordered &= bytes[i]==static_cast<std::uint8_t>(input);
        // Keep below the hard TX cap; write failure deliberately quarantines.
        if(output<count && output-received.load()<128) {
            auto byte=static_cast<std::uint8_t>(output++);
            if(!bridge.write(&byte,1)) ordered=false;
        }
        std::this_thread::yield();
    }
    while(received.load()<count) std::this_thread::yield();
    done.store(true); worker.join();
    CHECK(ordered); REQUIRE(device.output.size()==count);
    for(unsigned i=0;i<count;++i) if(device.output[i]!=static_cast<std::uint8_t>(i)) { CHECK(false); break; }
}
TEST_CASE("read captures session atomically and stale replies do not poison the new session") {
    UsbBridge<std::mutex,4,8> bridge; Device device;
    REQUIRE(bridge.attach()); bridge.connection(true); bridge.pump(device);
    device.input={1}; bridge.pump(device);
    std::uint8_t byte=0; std::uint32_t observed=0;
    REQUIRE(bridge.read(&byte,1,&observed)==1);
    REQUIRE(bridge.write_if_epoch(&byte,1,observed));
    bridge.reset_from_device(); device.clear_rx(); device.clear_tx();
    bridge.connection(true); bridge.clear_device(device);
    device.input={2}; bridge.pump(device);
    const auto old=observed;
    CHECK(bridge.read(&byte,0,&observed)==0); CHECK(observed==old);
    CHECK_FALSE(bridge.write_if_epoch(&byte,1,observed));
    REQUIRE(bridge.read(&byte,1,&observed)==1); CHECK(observed!=old);
    REQUIRE(bridge.write_if_epoch(&byte,1,observed)); bridge.pump(device);
    CHECK(device.output==std::vector<std::uint8_t>{2});
}
