#pragma once
// The mbedTLS callback behind install_trust, exposed so it can be tested against real mbedTLS.
#include "tls_trust.h"
#include "mbedtls/x509_crt.h"

namespace fujinet::platform::pico::detail {

// mbedtls_x509_crt_ca_cb_t. `ctx` is a TrustSources. Returns, through `candidates`, a heap list
// (owned by mbedTLS from then on) of the trusted certificates whose subject equals `child`'s
// issuer, or null if there are none. A root the library cannot parse is skipped, never trusted.
int ca_lookup(void* ctx, const mbedtls_x509_crt* child, mbedtls_x509_crt** candidates);

} // namespace fujinet::platform::pico::detail
