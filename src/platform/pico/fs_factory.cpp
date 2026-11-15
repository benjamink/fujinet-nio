#include "fujinet/platform/pico/fs_factory.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/http_filesystem.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/platform/network_registry.h"
#include "fujinet/platform/udp_socket_ops.h"
#include "fujinet/net/udp_channel.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"
#include "detail/tcp_protocol.h"
#include "pico/time.h"

namespace fujinet::platform::pico {

// TNFS's byte channel uses the portable TCP state machine, never BSD select.
class TnfsTcpChannel final : public io::Channel {
    TcpProtocol protocol_;
    std::uint32_t read_ = 0, write_ = 0;
public:
    TnfsTcpChannel(const std::string& host, std::uint16_t port) {
        io::NetworkOpenRequest r; r.url="tcp://"+host+":"+std::to_string(port);
        protocol_.open(r);
    }
    bool available() override { protocol_.poll(); return protocol_.read_ready(); }
    std::size_t read(std::uint8_t* p, std::size_t n) override {
        protocol_.poll(); std::uint16_t count=0; bool eof=false, more=false;
        protocol_.read_body(read_,p,n,count,eof,more); read_+=count; return count;
    }
    void write(const std::uint8_t* p, std::size_t n) override {
        // Channel's void write cannot report partial acceptance. Bound retries;
        // close on failure so a partially sent TNFS request cannot be reused.
        const auto deadline=make_timeout_time_ms(5000);
        while(n) {
            protocol_.poll(); std::uint16_t count=0;
            auto status=protocol_.write_body(write_,p,n,count);
            if(status != io::StatusCode::Ok && status != io::StatusCode::NotReady && status != io::StatusCode::DeviceBusy) { protocol_.close(); return; }
            write_+=count; p+=count; n-=count;
            if(!n) break;
            if(time_reached(deadline)) { protocol_.close(); return; }
            sleep_ms(1);
        }
    }
};
std::unique_ptr<fs::IFileSystem> create_tnfs_filesystem(bool useTcp)
{
    fs::TnfsClientFactory factory = [useTcp](const fs::TnfsEndpoint& endpoint) -> std::unique_ptr<tnfs::ITnfsClient> {
        if(useTcp || endpoint.useTcp) return tnfs::make_tcp_tnfs_client(std::make_unique<TnfsTcpChannel>(endpoint.host,endpoint.port));
        return tnfs::make_udp_tnfs_client(std::make_unique<net::UdpChannel>(default_udp_socket_ops(),endpoint.host,endpoint.port));
    };
    return fs::make_tnfs_filesystem(std::move(factory));
}

std::unique_ptr<fs::IFileSystem> create_http_filesystem()
{
    auto registry = std::make_shared<io::ProtocolRegistry>(make_default_network_registry());
    fs::HttpProtocolFactory factory = [registry](std::string_view scheme) { return registry->create(scheme); };
    return fs::make_http_filesystem(std::move(factory));
}

} // namespace fujinet::platform::pico
