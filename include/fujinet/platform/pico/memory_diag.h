#pragma once
// Pico memory high-water marks for the `pico.mem` console command: how deep each core's stack has
// gone, and the peaks of the two heaps (newlib's, and lwIP's, which mbedTLS allocates from).
#include "fujinet/diag/diagnostic_provider.h"

#include <memory>

namespace fujinet::platform::pico {

std::unique_ptr<diag::IDiagnosticProvider> create_memory_diagnostic_provider();

} // namespace fujinet::platform::pico
