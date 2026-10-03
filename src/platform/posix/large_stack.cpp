#include "fujinet/platform/large_stack.h"

#include <pthread.h>

#include <algorithm>
#include <climits>

namespace fujinet::platform {

namespace {

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

} // namespace

// A real thread with exactly the requested stack, rather than a direct call
// on the (8 MB) main stack, so host tests overflow where a target would.
bool run_with_large_stack(std::size_t stackBytes, void (*fn)(void* ctx), void* ctx)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    const std::size_t size = std::max<std::size_t>(stackBytes, PTHREAD_STACK_MIN);
    if (pthread_attr_setstacksize(&attr, size) != 0) {
        pthread_attr_destroy(&attr);
        return false;
    }

    Job job{fn, ctx};
    pthread_t thread;
    const int created = pthread_create(&thread, &attr, &run_job, &job);
    pthread_attr_destroy(&attr);
    if (created != 0) {
        return false;
    }
    pthread_join(thread, nullptr);
    return true;
}

} // namespace fujinet::platform
