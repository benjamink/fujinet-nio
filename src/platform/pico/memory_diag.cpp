#include "fujinet/platform/pico/memory_diag.h"
#include "fujinet/platform/pico/stack_probe.h"

#include "lwip/stats.h"
#include "pico/cyw43_arch.h"

#include <cstddef>
#include <cstring>
#include <malloc.h>
#include <string>
#include <unistd.h>

// Linker-provided: core 0's stack is [__StackBottom, __StackTop), core 1's is
// [__StackOneBottom, __StackOneTop), and newlib's heap starts at __end__.
extern "C" char __StackBottom, __end__;

namespace fujinet::platform::pico {
namespace {

class MemoryProvider final : public diag::IDiagnosticProvider {
public:
    std::string_view provider_id() const noexcept override { return "pico"; }

    void list_commands(std::vector<diag::DiagCommandSpec>& out) const override
    {
        out.push_back(diag::DiagCommandSpec{
            .name = "pico.mem",
            .summary = "stack and heap high-water marks (both cores' stacks, newlib heap, lwIP/TLS heap)",
            .usage = "pico.mem",
        });
    }

    diag::DiagResult execute(const diag::DiagArgsView& args) override
    {
        if (args.argv.empty() || args.argv[0] != "pico.mem") return diag::DiagResult::not_found("unknown pico command");

        diag::DiagResult r = diag::DiagResult::ok();
        auto line = [&r](const char* key, std::size_t value) {
            r.text += key;
            r.text += ": ";
            r.text += std::to_string(value);
            r.text += "\r\n";
            r.kv.emplace_back(key, std::to_string(value));
        };

        const StackRegion stack0 = core0_stack_region(), stack1 = core1_stack_region();
        line("core0_stack_size", static_cast<std::size_t>(stack0.top - stack0.bottom));
        line("core0_stack_min_free", stack_min_free(stack0));
        line("core1_stack_size", static_cast<std::size_t>(stack1.top - stack1.bottom));
        line("core1_stack_min_free", stack_min_free(stack1));

        // newlib's arena only grows, so what it has taken from the system is its high-water mark.
        const struct mallinfo mi = mallinfo();
        const std::size_t heap_total = static_cast<std::size_t>(&__StackBottom - &__end__);
        line("heap_total", heap_total);
        line("heap_arena_peak", static_cast<std::size_t>(mi.arena));
        line("heap_in_use", static_cast<std::size_t>(mi.uordblks));

        // lwIP's heap holds the pbufs' payloads that are not pooled, and every mbedTLS allocation.
        cyw43_arch_lwip_begin();
        const std::size_t avail = lwip_stats.mem.avail, used = lwip_stats.mem.used, peak = lwip_stats.mem.max,
                          failures = lwip_stats.mem.err;
        cyw43_arch_lwip_end();
        line("lwip_heap_size", avail);
        line("lwip_heap_in_use", used);
        line("lwip_heap_peak", peak);
        line("lwip_heap_alloc_failures", failures);
        return r;
    }
};

} // namespace

std::unique_ptr<diag::IDiagnosticProvider> create_memory_diagnostic_provider()
{
    return std::make_unique<MemoryProvider>();
}

} // namespace fujinet::platform::pico
