#include "fujinet/console/console_engine.h"
#include "fujinet/platform/pico/usb_service.h"
#include "detail/socket_state.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
namespace {
fujinet::platform::pico::detail::ByteRing<8192> output;
bool truncated=false;
class UartConsole final : public fujinet::console::IConsoleTransport {
public:
    bool read_byte(std::uint8_t& out,int) override {
        if(!uart_is_readable(uart0)) return false;
        out=uart_getc(uart0); return true;
    }
    void write(std::string_view s) override {
        const auto count=std::min(s.size(),output.free());
        if(count < s.size()) truncated=true;
        output.push(s.data(),count); // bounded diagnostics output; never stall the core
    }
    void write_line(std::string_view s) override { write(s); write("\r\n"); }
};
}
namespace fujinet::console {
std::unique_ptr<IConsoleTransport> create_default_console_transport() { return std::make_unique<UartConsole>(); }
}
namespace fujinet::platform::pico {
void service_console() {
    constexpr char marker[]="\r\n[console output truncated]\r\n";
    if(truncated && output.free() >= sizeof(marker)-1) {
        output.push(marker,sizeof(marker)-1); truncated=false;
    }
    for(unsigned i=0;i<64 && output.size() && uart_is_writable(uart0);++i) {
        std::uint8_t c; output.pop(&c,1); uart_putc_raw(uart0,c);
    }
}
}
