#pragma once
#include <ctime>
using mbedtls_time_t=std::time_t;
inline int mbedtls_platform_set_time(mbedtls_time_t(*)(mbedtls_time_t*)) { return 0; }
