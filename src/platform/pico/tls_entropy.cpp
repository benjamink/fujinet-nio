#include "detail/entropy.h"
#include "pico/rand.h"
extern "C" int mbedtls_hardware_poll(void*,unsigned char* out,std::size_t length,std::size_t* written) {
    if(!written || (!out && length)) return -1;
    *written=0;
    fujinet::platform::pico::detail::fill_entropy(out,length,[] { return get_rand_64(); });
    *written=length; return 0;
}

#include "pico/time.h"
#include "mbedtls/platform_time.h"
extern "C" mbedtls_ms_time_t mbedtls_ms_time() { return time_us_64()/1000; }
