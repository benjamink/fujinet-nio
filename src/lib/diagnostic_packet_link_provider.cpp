#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/io/devices/fuji_device.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

void line(std::string& out, std::string_view key, std::string_view value)
{
    out += key;
    out += ": ";
    out += value;
    out += "\r\n";
}

void line(std::string& out, std::string_view key, std::uint64_t value)
{
    line(out, key, std::to_string(value));
}

class PacketLinkDiagnosticProvider final : public IDiagnosticProvider {
public:
    PacketLinkDiagnosticProvider(io::PacketLinkChannel& channel, io::FujiDevice* fuji)
        : _channel(channel)
        , _fuji(fuji)
    {}

    std::string_view provider_id() const noexcept override { return "link"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "link.status",
            .summary = "show the packet link's state, settings and counters",
            .usage = "link.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "link.set",
            .summary = "set record_timeout_ms (now; controllers learn it at the next Sync) "
                       "or capacity (stored; applies after restart)",
            .usage = "link.set <record_timeout_ms|capacity> <value>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "link.save",
            .summary = "write the packet link settings into fujinet.yaml (requires FujiDevice)",
            .usage = "link.save",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        const std::string_view cmd = args.argv[0];
        if (cmd == "link.status") return cmd_status();
        if (cmd == "link.set") return cmd_set(args);
        if (cmd == "link.save") return cmd_save();
        return DiagResult::not_found("unknown link command");
    }

private:
    DiagResult cmd_status()
    {
        const io::PacketLink& link = _channel.link();
        const auto& st = link.stats();
        std::string text;
        line(text, "state", link.state_name());
        line(text, "generation", link.generation());
        line(text, "version", link.version());
        line(text, "capacity", link.capacity());
        line(text, "record_timeout_ms", link.record_timeout_ms());
        if (_fuji) {
            const auto& stored = _fuji->config().channel.packetLink;
            line(text, "stored_capacity", stored.capacity == 0 ? std::string("default") : std::to_string(stored.capacity));
            line(text, "stored_record_timeout_ms",
                 stored.recordTimeoutMs == 0 ? std::string("auto") : std::to_string(stored.recordTimeoutMs));
        }
        line(text, "syncs", st.syncs);
        line(text, "requests", st.requests);
        line(text, "answers", st.answers);
        line(text, "unanswered", st.unanswered);
        line(text, "refused", st.refused);
        line(text, "corrupt", st.corrupt);
        line(text, "oversized", st.oversized);
        line(text, "abandoned", st.abandoned);
        return DiagResult::ok(std::move(text));
    }

    DiagResult cmd_set(const DiagArgsView& args)
    {
        if (args.argv.size() < 3) {
            return DiagResult::invalid_args("usage: link.set <record_timeout_ms|capacity> <value>");
        }
        const std::string_view field = args.argv[1];
        std::uint32_t value = 0;
        if (!parse_decimal_u32(args.argv[2], value)) {
            return DiagResult::invalid_args("value must be a number");
        }

        if (ascii_iequals(field, "record_timeout_ms")) {
            if (value < 1 || value > io::PacketLink::kMaxRecordTimeoutMs) {
                return DiagResult::invalid_args("record_timeout_ms must be 1.." +
                                                std::to_string(io::PacketLink::kMaxRecordTimeoutMs));
            }
            _channel.link().set_record_timeout_ms(value);
            if (_fuji) _fuji->config_mut().channel.packetLink.recordTimeoutMs = value;
            return DiagResult::ok("record_timeout_ms set to " + std::to_string(value) +
                                  " (controllers learn it at the next Sync)");
        }
        if (ascii_iequals(field, "capacity")) {
            if (value < io::PacketLink::kMinCapacity || value > io::PacketLink::kMaxCapacity) {
                return DiagResult::invalid_args("capacity must be " + std::to_string(io::PacketLink::kMinCapacity) +
                                                ".." + std::to_string(io::PacketLink::kMaxCapacity));
            }
            if (!_fuji) return DiagResult::not_ready("FujiDevice not available to store capacity");
            _fuji->config_mut().channel.packetLink.capacity = value;
            return DiagResult::ok("capacity " + std::to_string(value) +
                                  " stored; link.save, then restart to apply");
        }
        return DiagResult::invalid_args("unknown field (record_timeout_ms or capacity)");
    }

    DiagResult cmd_save()
    {
        if (!_fuji) return DiagResult::not_ready("FujiDevice not available");
        auto* store = _fuji->config_store();
        if (!store) return DiagResult::not_ready("config store not available");
        store->save(_fuji->config());
        return DiagResult::ok("saved channel.packet_link to config store");
    }

    io::PacketLinkChannel& _channel;
    io::FujiDevice* _fuji;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_packet_link_diagnostic_provider(
    io::Channel* channel,
    io::FujiDevice* fuji)
{
    auto* link = dynamic_cast<io::PacketLinkChannel*>(channel);
    if (link == nullptr) return nullptr;
    return std::make_unique<PacketLinkDiagnosticProvider>(*link, fuji);
}

} // namespace fujinet::diag
