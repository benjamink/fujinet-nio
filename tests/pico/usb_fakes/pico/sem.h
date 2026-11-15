#pragma once
struct semaphore_t { int count=0; };
void sem_init(semaphore_t*,int,int);
void sem_release(semaphore_t*);
bool sem_acquire_timeout_ms(semaphore_t*,unsigned);
