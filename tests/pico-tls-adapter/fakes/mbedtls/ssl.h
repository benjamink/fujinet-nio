#pragma once
#include <string>
struct mbedtls_ssl_context { std::string hostname; unsigned long verify=0; };
inline unsigned long mbedtls_ssl_get_verify_result(const mbedtls_ssl_context* ssl) { return ssl->verify; }
inline int mbedtls_ssl_set_hostname(mbedtls_ssl_context* ssl,const char* host) { ssl->hostname=host; return 0; }
