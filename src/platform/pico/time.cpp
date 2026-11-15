#include "fujinet/platform/time.h"

#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pico/time.h"
#include "pico/cyw43_arch.h"

namespace fujinet::platform {
namespace {
std::uint64_t epoch_seconds=0, epoch_micros=0;
}
std::uint64_t unix_time_seconds() {
    cyw43_arch_lwip_begin();
    const auto value=epoch_seconds ? epoch_seconds+(time_us_64()-epoch_micros)/1000000 : 0;
    cyw43_arch_lwip_end();
    return value;
}
bool set_unix_time_seconds(std::uint64_t seconds) {
    if(seconds<1577836800ULL || seconds>4102444800ULL) return false;
    cyw43_arch_lwip_begin(); epoch_seconds=seconds; epoch_micros=time_us_64(); cyw43_arch_lwip_end();
    return true;
}
bool time_is_valid() { return unix_time_seconds()>=1577836800ULL; }
bool sync_network_time() { return false; } // ClockService SetTime is the explicit prerequisite.

static bool gmtime_utc(std::uint64_t unix_seconds, tm& out_tm)
{
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm tm{};

#if defined(__unix__) || defined(__APPLE__)
    if (::gmtime_r(&t, &tm) == nullptr) {
        return false;
    }
#else
    // Best-effort fallback if gmtime_r is unavailable.
    struct tm* p = std::gmtime(&t); // "struct": the local `tm tm` hides the type
    if (!p) return false;
    tm = *p;
#endif

    out_tm = tm;
    return true;
}

bool format_time_utc_iso8601(std::uint64_t unix_seconds, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0) return false;
    tm tm{};
    if (!gmtime_utc(unix_seconds, tm)) return false;
    return ::strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm) != 0;
}

bool format_time_utc_ls(std::uint64_t unix_seconds, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0) return false;
    tm tm{};
    if (!gmtime_utc(unix_seconds, tm)) return false;
    return ::strftime(out, out_len, "%b %e %H:%M", &tm) != 0;
}

static bool localtime_from_unix(std::uint64_t unix_seconds, tm& out_tm)
{
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm tm{};

#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&t, &tm) == nullptr) {
        return false;
    }
#else
    struct tm* p = std::localtime(&t); // "struct": the local `tm tm` hides the type
    if (!p) return false;
    tm = *p;
#endif

    out_tm = tm;
    return true;
}

bool format_time_local_ls(std::uint64_t unix_seconds, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0) return false;

    tm file_tm{};
    if (!localtime_from_unix(unix_seconds, file_tm)) return false;

    const std::time_t now_t = static_cast<std::time_t>(unix_time_seconds());
    tm now_tm{};
    if (!localtime_from_unix(static_cast<std::uint64_t>(now_t), now_tm)) {
        return ::strftime(out, out_len, "%b %e %H:%M", &file_tm) != 0;
    }

    if (file_tm.tm_year == now_tm.tm_year) {
        return ::strftime(out, out_len, "%b %e %H:%M", &file_tm) != 0;
    }
    return ::strftime(out, out_len, "%b %e  %Y", &file_tm) != 0;
}

// ============================================================================
// Timezone Support
// ============================================================================

bool set_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return false;
    }
    ::setenv("TZ", posix_tz, 1);
    ::tzset();
    return true;
}

std::string get_timezone()
{
    const char* tz = std::getenv("TZ");
    return tz ? std::string(tz) : std::string("UTC");
}

bool validate_timezone(const char* posix_tz)
{
    if (!posix_tz || posix_tz[0] == '\0') {
        return false;
    }
    
    // Try to apply the timezone temporarily and see if it works
    // by checking if localtime produces valid results
    ScopedTimezone scoped_tz(posix_tz);
    
    std::time_t now = static_cast<std::time_t>(unix_time_seconds());
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&now, &local_tm) == nullptr) {
        return false;
    }
#else
    tm* p = std::localtime(&now);
    if (!p) return false;
    local_tm = *p;
#endif
    
    // If we got here, the timezone was accepted
    return true;
}

bool get_local_time(std::uint64_t unix_seconds, const char* tz, LocalTime& out)
{
    if (!tz || tz[0] == '\0') {
        return false;
    }
    
    ScopedTimezone scoped_tz(tz);
    
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
#else
    struct tm* p = std::localtime(&t); // "struct": the local `tm tm` hides the type
    if (!p) return false;
    local_tm = *p;
#endif
    
    out.year = local_tm.tm_year + 1900;
    out.month = local_tm.tm_mon + 1;
    out.day = local_tm.tm_mday;
    out.hour = local_tm.tm_hour;
    out.minute = local_tm.tm_min;
    out.second = local_tm.tm_sec;
    out.weekday = local_tm.tm_wday;
    out.yearday = local_tm.tm_yday;
    out.is_dst = local_tm.tm_isdst > 0;
    
    return true;
}

bool format_time_local_iso8601(std::uint64_t unix_seconds, const char* tz, char* out, std::size_t out_len)
{
    if (!out || out_len == 0 || unix_seconds == 0 || !tz || tz[0] == '\0') {
        return false;
    }
    
    ScopedTimezone scoped_tz(tz);
    
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    tm local_tm{};
    
#if defined(__unix__) || defined(__APPLE__)
    if (::localtime_r(&t, &local_tm) == nullptr) {
        return false;
    }
#else
    struct tm* p = std::localtime(&t); // "struct": the local `tm tm` hides the type
    if (!p) return false;
    local_tm = *p;
#endif
    
    // Format: YYYY-MM-DDTHH:MM:SS+HHMM
    return ::strftime(out, out_len, "%FT%T%z", &local_tm) != 0;
}

} // namespace fujinet::platform
