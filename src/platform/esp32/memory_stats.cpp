#include "fujinet/platform/memory_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

namespace fujinet::platform {

MemoryStats memory_stats()
{
    MemoryStats m;
    m.available = true;

    m.internalFreeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.internalMinFreeBytes = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    m.internalLargestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    m.psramFreeBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.psramMinFreeBytes = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    m.psramLargestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    m.taskName = pcTaskGetName(nullptr);
    // StackType_t is a byte on ESP-IDF, so the high-water mark is in bytes.
    m.taskStackMinFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
    return m;
}

} // namespace fujinet::platform
