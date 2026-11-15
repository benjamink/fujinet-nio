#include "../../../pico2w/lwipopts.h"
// Host pointers require 8-byte alignment; keep the production heap budget.
#undef MEM_ALIGNMENT
#define MEM_ALIGNMENT 8
#define SYS_LIGHTWEIGHT_PROT 0
#undef MEM_STATS
#define MEM_STATS 1
#define LWIP_STATS 1
#ifdef TEST_OLD_HEAP
#undef MEM_SIZE
#define MEM_SIZE (24 * 1024)
#endif
