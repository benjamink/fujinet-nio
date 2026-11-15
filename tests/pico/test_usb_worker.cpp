#include "fujinet/platform/pico/usb_service.h"
#include "fujinet/platform/channel_factory.h"
#include "../../src/platform/pico/detail/usb_worker.h"
#include "pico/sem.h"
#include "device/usbd_pvt.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
namespace fujinet::config { struct FujiConfig {}; }
extern "C" void tud_mount_cb();
extern "C" void tud_umount_cb();
extern "C" void tud_cdc_line_state_cb(std::uint8_t,bool,bool);
namespace {
int core=0, init_calls=0, clear_rx=0, clear_tx=0;
bool reconnect_in_task=false, reset_in_task=false;
const usbd_class_driver_t* observer=nullptr;
bool flash_ok=true, usb_ok=true, connected=false, timeout=false, flash_registered=false;
std::deque<unsigned char> incoming;
std::vector<unsigned char> outgoing;
struct StopWorker {};
void usb_core() { assert(core==1); }
void step() { core=1; fujinet::platform::pico::detail::usb_worker_step(); core=0; }
}
bool flash_safe_execute_core_init() { usb_core(); flash_registered=flash_ok; return flash_ok; }
bool tusb_init() {
    usb_core(); assert(flash_registered); ++init_calls;
    std::uint8_t count=0; observer=usbd_app_driver_get_cb(&count);
    assert(observer && count==1); observer->init();
    // Observer must never take an interface from the actual CDC class driver.
    assert(observer->open(0,nullptr,0)==0);
    return usb_ok;
}
void tud_task() {
    usb_core();
    if(reset_in_task) {
        reset_in_task=false;
        // SDK configuration_reset iterates app drivers, then CDC. Model that
        // exact task ordering, including a reset queued while tud_task runs.
        observer->reset(0);
        connected=false; incoming.clear(); outgoing.clear(); // CDC reset
        if(reconnect_in_task) {
            tud_mount_cb(); connected=true;
        }
    }
    if(reconnect_in_task) {
        reconnect_in_task=false;
        tud_cdc_line_state_cb(0,false,false);
        tud_cdc_line_state_cb(0,true,false);
        incoming={42,43};
    }
}
bool tud_cdc_connected() { usb_core(); return connected; }
void tud_cdc_read_flush() { usb_core(); incoming.clear(); ++clear_rx; }
void tud_cdc_write_clear() { usb_core(); outgoing.clear(); ++clear_tx; }
std::uint32_t tud_cdc_read(void* p,std::uint32_t n) {
    usb_core(); unsigned count=0;
    while(count<n && !incoming.empty()) {
        static_cast<unsigned char*>(p)[count++]=incoming.front(); incoming.pop_front();
    }
    return count;
}
std::uint32_t tud_cdc_write_available() { usb_core(); return 64; }
std::uint32_t tud_cdc_write(const void* p,std::uint32_t n) {
    usb_core(); auto b=static_cast<const unsigned char*>(p);
    outgoing.insert(outgoing.end(),b,b+n); return n;
}
std::uint32_t tud_cdc_write_flush() { usb_core(); return 0; }
void sem_init(semaphore_t* s,int count,int) { s->count=count; }
void sem_release(semaphore_t* s) { ++s->count; }
bool sem_acquire_timeout_ms(semaphore_t* s,unsigned ms) {
    assert(core==0 && ms==2000);
    return !timeout && s->count>0;
}
void tight_loop_contents() { throw StopWorker{}; }
void multicore_launch_core1(void (*entry)()) {
    assert(core==0); if(timeout) return;
    core=1; try { entry(); } catch(StopWorker&) {} core=0;
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    flash_ok=mode!="flash-failure"; usb_ok=mode!="usb-failure"; timeout=mode=="timeout";
    using namespace fujinet::platform;
    const bool started=pico::start_usb_worker();
    assert(started==(mode=="lifecycle"));
    assert(!pico::start_usb_worker());
    if(!started) {
        assert(init_calls==((flash_ok && !timeout)?1:0));
        return 0;
    }
    fujinet::build::BuildProfile profile{};
    profile.primaryChannel=fujinet::build::ChannelKind::UsbCdcDevice;
    fujinet::config::FujiConfig config;
    auto channel=create_channel_for_profile(profile,config);
    assert(channel);
    assert(!create_channel_for_profile(profile,config));
    connected=true; step();
    incoming={1,2,3}; step();
    unsigned char data[8]{};
    assert(channel->available());
    assert(channel->read(data,8)==3 && data[0]==1 && data[2]==3);
    channel->write(data,3); step(); assert(outgoing.size()==3);
    auto epoch=channel->connection_epoch();
    core=1; tud_cdc_line_state_cb(0,false,false); core=0;
    connected=false; step();
    assert(channel->connection_epoch()!=epoch && !channel->available() && outgoing.empty());
    connected=true; step(); incoming={4,5}; step();
    epoch=channel->connection_epoch();
    reset_in_task=true;
    // Queued reset changes state only when TinyUSB dispatches it on core 1.
    assert(channel->connection_epoch()==epoch);
    step();
    assert(channel->connection_epoch()!=epoch && !channel->available());
    core=1; tud_mount_cb(); tud_umount_cb(); core=0;
    connected=true; step(); incoming={6,7}; step();
    assert(channel->available());
    channel.reset();
    channel=create_channel_for_profile(profile,config);
    assert(channel && !channel->available());
    step(); incoming={8}; step();
    assert(channel->read(data,8)==1 && data[0]==8);
    channel->write(data,1);
    reconnect_in_task=true; step();
    assert(outgoing.empty());
    assert(channel->read(data,8)==2 && data[0]==42 && data[1]==43);
    // A reset arriving DURING tud_task, followed by mount/DTR/RX in that same
    // dispatch, must not clear the fresh bytes after tud_task returns.
    channel->write(data,2);
    reset_in_task=true; reconnect_in_task=true; step();
    assert(outgoing.empty());
    // Slow response from the previous read must not leak into this session.
    channel->write(data,2); step(); assert(outgoing.empty());
    (void)channel->connection_epoch();
    channel->write(data,2); step(); assert(outgoing.empty());
    // Nor may an empty read grant permission to reply in the new session.
    assert(channel->read(data,0)==0);
    channel->write(data,2); step(); assert(outgoing.empty());
    assert(channel->read(data,8)==2 && data[0]==42 && data[1]==43);
    channel->write(data,2); step();
    assert(outgoing.size()==2 && outgoing[0]==42 && outgoing[1]==43);
    assert(clear_rx>0 && clear_tx>0);
}
