#include "fujinet/io/devices/fuji_device.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/version.h"
#include "fujinet/io/devices/fuji_commands.h"

#include <string_view>

namespace fujinet::io {

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::io::protocol::FujiCommand;
using fujinet::io::protocol::to_fuji_command;

namespace {

constexpr std::uint8_t INFO_VERSION = 1;
constexpr std::size_t MAX_FIRMWARE_VERSION = 32;
constexpr std::size_t MAX_PROFILE_NAME = 64;

void put_string8(std::vector<std::uint8_t>& out, std::string_view text, std::size_t max)
{
    text = text.substr(0, max);
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

} // namespace

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
{
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    switch (to_fuji_command(request.command)) {
        case FujiCommand::GetInfo:
            return handle_get_info(request);
        case FujiCommand::Reset:
            return handle_reset(request);
        default:
            return handle_unknown(request);
    }
}

void FujiDevice::poll()
{
    // Background work later (autosave, timers, etc).
}

void FujiDevice::start()
{
    load_config();
}

IOResponse FujiDevice::handle_get_info(const IORequest& request)
{
    if (request.payload.size() != 1 || request.payload[0] != INFO_VERSION) {
        return make_base_response(request, StatusCode::InvalidRequest);
    }
    auto resp = make_success_response(request);
    resp.payload = {INFO_VERSION};
    put_string8(resp.payload, fujinet::version(), MAX_FIRMWARE_VERSION);
    put_string8(resp.payload, build::current_build_profile().name, MAX_PROFILE_NAME);
    return resp;
}

IOResponse FujiDevice::handle_reset(const IORequest& request)
{
    auto resp = make_success_response(request);

    if (_resetHandler) {
        _resetHandler();
    }

    return resp;
}

IOResponse FujiDevice::handle_unknown(const IORequest& request)
{
    return make_base_response(request, StatusCode::Unsupported);
}

void FujiDevice::load_config()
{
    if (_configStore) {
        _config = _configStore->load();
    }
}

} // namespace fujinet::io
