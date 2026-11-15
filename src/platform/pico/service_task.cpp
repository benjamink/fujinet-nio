#include "fujinet/platform/service_task.h"

namespace fujinet::platform {

// No threads without an RTOS or core-1 launch: services that block for long
// (HotSync) are not started on this platform yet.
bool start_service_task(const char* /*name*/, std::size_t /*stackBytes*/, void (* /*fn*/)(void*), void* /*ctx*/)
{
    return false;
}

} // namespace fujinet::platform
