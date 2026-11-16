#include "detail/tls_trust.h"
#include "detail/tls_trust_lookup.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"

namespace fujinet::platform::pico::detail {

int ca_lookup(void* ctx, const mbedtls_x509_crt* child, mbedtls_x509_crt** candidates) {
    *candidates = nullptr;
    const auto* sources = static_cast<const TrustSources*>(ctx);
    if (!sources || !child) return 0;

    auto* head = static_cast<mbedtls_x509_crt*>(mbedtls_calloc(1, sizeof(mbedtls_x509_crt)));
    if (!head) return MBEDTLS_ERR_X509_ALLOC_FAILED;
    mbedtls_x509_crt_init(head);

    bool any = false;
    if (sources->bundle) {
        // A root that does not parse is not a candidate; the others still are.
        sources->bundle->find_subject(child->issuer_raw.p, child->issuer_raw.len,
                                      [&](CaBundle::Root root) {
            if (mbedtls_x509_crt_parse_der(head, root.der, root.der_len) == 0) any = true;
        });
    }
    if (sources->extra_pem && sources->extra_pem_len) {
        // mbedTLS checks each candidate's name and signature itself, so all of them are offered.
        // The parse returns how many certificates failed (positive) or a hard error (negative):
        // a PEM with one unusable certificate still offers the rest, but only a certificate that
        // really parsed (version set) can be a candidate.
        const int parsed = mbedtls_x509_crt_parse(head, reinterpret_cast<const unsigned char*>(sources->extra_pem),
                                                  sources->extra_pem_len);
        if (parsed >= 0 && head->version != 0) any = true;
    }
    if (!any) {
        mbedtls_x509_crt_free(head);
        mbedtls_free(head);
        return 0;
    }
    *candidates = head;
    return 0;
}

bool install_trust(mbedtls_ssl_context* ssl, const TrustSources& sources) {
    if (!ssl) return false;
    // The adapter builds one config per connection and frees it with the connection, so this
    // changes nothing shared.
    auto* config = const_cast<mbedtls_ssl_config*>(ssl->MBEDTLS_PRIVATE(conf));
    if (!config) return false;
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(config, ca_lookup, const_cast<TrustSources*>(&sources));
    return true;
}

} // namespace fujinet::platform::pico::detail
