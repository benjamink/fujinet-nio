// libc pieces newlib leaves to the platform.
//
// libstdc++'s std::this_thread::sleep_for calls sleep()/usleep(), which
// newlib/arm-none-eabi does not provide. Back them with the Pico SDK timers.
#include <sys/types.h>

#include "pico/time.h"

extern "C" {

int usleep(useconds_t us)
{
    sleep_us(us);
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    sleep_ms(seconds * 1000u);
    return 0;
}

} // extern "C"
