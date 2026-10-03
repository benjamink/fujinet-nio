#include "fujinet/platform/large_stack.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

namespace fujinet::platform {

namespace {

struct Job {
    void (*fn)(void*);
    void* ctx;
    SemaphoreHandle_t done;
};

void run_job(void* arg)
{
    auto* job = static_cast<Job*>(arg);
    job->fn(job->ctx);
    xSemaphoreGive(job->done);
    // The caller deletes this task (vTaskDeleteWithCaps frees the PSRAM
    // stack, which a task cannot do for itself). Wait here until it does.
    vTaskSuspend(nullptr);
}

} // namespace

// A temporary task whose stack is in PSRAM (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM,
// on by default for an S3 with PSRAM), so the large stack costs no internal
// RAM and only exists for this call. Without PSRAM the task cannot be made
// and this returns false.
// The caller blocks until it finishes, so the work stays synchronous.
// The work must not write flash: a task with an external stack cannot run
// while the flash cache is disabled.
bool run_with_large_stack(std::size_t stackBytes, void (*fn)(void* ctx), void* ctx)
{
    Job job{fn, ctx, xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        return false;
    }

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreateWithCaps(&run_job,
                                                   "fn_large_stack",
                                                   static_cast<uint32_t>(stackBytes),   // bytes in ESP-IDF
                                                   &job,
                                                   uxTaskPriorityGet(nullptr),
                                                   &task,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        vSemaphoreDelete(job.done);
        return false;
    }

    xSemaphoreTake(job.done, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    vSemaphoreDelete(job.done);
    return true;
}

} // namespace fujinet::platform
