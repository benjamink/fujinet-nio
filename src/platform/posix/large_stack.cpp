#include "fujinet/platform/large_stack.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

namespace fujinet::platform {

namespace {

constexpr std::uint8_t kPaint = 0xA5;

struct Job {
    void (*fn)(void*);
    void* ctx;
};

void* run_job(void* arg)
{
    auto* job = static_cast<Job*>(arg);
    job->fn(job->ctx);
    return nullptr;
}

// Bytes at the low end of the stack the thread never wrote. The stack grows
// down on every host we build for, so that is the part it never reached.
std::size_t untouched_bytes(const std::uint8_t* stack, std::size_t size)
{
    std::size_t n = 0;
    while (n < size && stack[n] == kPaint) {
        ++n;
    }
    return n;
}

} // namespace

// A real thread with exactly the requested stack, rather than a direct call
// on the (8 MB) main stack, so host tests overflow where a target would. The
// stack has a guard page below it, so an overflow still faults, and is
// painted so the depth reached can be reported. Heap use is not measured.
bool run_with_large_stack(std::size_t stackBytes,
                          void (*fn)(void* ctx),
                          void* ctx,
                          core::LargeStackReport* report)
{
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    std::size_t size = std::max<std::size_t>(stackBytes, PTHREAD_STACK_MIN);
    size = (size + page - 1) / page * page;

    void* mapping = mmap(nullptr, size + page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }
    auto* guard = static_cast<std::uint8_t*>(mapping);
    std::uint8_t* stack = guard + page;
    if (mprotect(guard, page, PROT_NONE) != 0) {
        munmap(mapping, size + page);
        return false;
    }
    std::memset(stack, kPaint, size);

    pthread_attr_t attr;
    bool ran = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstack(&attr, stack, size) == 0) {
            Job job{fn, ctx};
            pthread_t thread;
            if (pthread_create(&thread, &attr, &run_job, &job) == 0) {
                pthread_join(thread, nullptr);
                ran = true;
            }
        }
        pthread_attr_destroy(&attr);
    }

    if (ran && report != nullptr) {
        *report = core::LargeStackReport{};
        report->stackUsedBytes = size - untouched_bytes(stack, size);
    }
    munmap(mapping, size + page);
    return ran;
}

} // namespace fujinet::platform
