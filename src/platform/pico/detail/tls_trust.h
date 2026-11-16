#pragma once
// How a TLS connection finds the roots that anchor a server's certificate chain.
//
// mbedTLS is given a callback instead of a parsed list of roots. While it verifies a chain
// it asks "which trusted certificates could have signed this one?", and the answer is built on
// demand from the flash bundle (see ca_bundle.h) by looking the issuer's name up. Only those
// few certificates are parsed, so the cost is one or two certificates of heap per handshake
// and not the whole set.
#include "ca_bundle.h"
#include <cstddef>

struct mbedtls_ssl_context;

namespace fujinet::platform::pico::detail {

// What may anchor a chain. Must outlive the handshake.
struct TrustSources {
    const CaBundle* bundle = nullptr;       // the platform roots, or null
    const char* extra_pem = nullptr;        // an extra PEM (the FujiNet test CA), or null
    std::size_t extra_pem_len = 0;          // including the terminating NUL mbedTLS needs
};

// Makes verification mandatory on the connection and anchors it in `sources`. Call after the
// connection is created and before the handshake. False if the connection has no config.
bool install_trust(mbedtls_ssl_context* ssl, const TrustSources& sources);

} // namespace fujinet::platform::pico::detail
