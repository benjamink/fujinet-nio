#pragma once
#include <mutex>
using mutex_t=std::mutex;
inline void mutex_init(mutex_t*) {}
inline void mutex_enter_blocking(mutex_t* m) { m->lock(); }
inline void mutex_exit(mutex_t* m) { m->unlock(); }
